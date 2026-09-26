// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

// culprit-helper — privileged kernel tracing helper for Culprit.
//
// Launched by the GUI through pkexec. Reads line commands on stdin, writes
// JSON lines on stdout, exits on stdin EOF / quit / SIGTERM. Accepts no file
// paths from its input and only writes to two whitelisted sysctls.
//
//   stdin:  start <features> [thresh_us=N]   features: sched,irq,softirq,reclaim,block,dstate,pio
//           stop | window <id> <cpu> <t0_ns> <t1_ns> | focus <pid>|none
//           sysctl <delayacct|schedstats> <on|off> | quit
//   stdout: {"t":"hello",...} {"t":"rqlat",...} {"t":"window",...} ... {"t":"bye"}

#include "common/fs/File.h"
#include "common/parse/Text.h"
#include "common/util/Clock.h"
#include "common/util/JsonWriter.h"
#include "helper/PerfTracer.h"
#include "helper/PrivProc.h"
#include "helper/SchedTracker.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace culprit;

namespace {

constexpr int kProto = 1;

bool writeAll(int fd, const std::string& s)
{
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::write(fd, s.data() + off, s.size() - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        off += size_t(n);
    }
    return true;
}

std::string findTracefs()
{
    struct stat st {};
    for (const char* p : {"/sys/kernel/tracing", "/sys/kernel/debug/tracing"}) {
        const std::string ev = std::string(p) + "/events/sched/sched_switch/id";
        if (::stat(ev.c_str(), &st) == 0)
            return p;
    }
    // Not mounted: mount tracefs at its standard location.
    if (::mount("tracefs", "/sys/kernel/tracing", "tracefs", 0, nullptr) == 0)
        return "/sys/kernel/tracing";
    return {};
}

std::vector<std::pair<std::string, std::string>> tracepointsFor(const std::vector<std::string>& features)
{
    std::vector<std::pair<std::string, std::string>> tps;
    auto has = [&](const char* f) { return std::find(features.begin(), features.end(), f) != features.end(); };
    if (has("sched")) {
        tps.push_back({"sched", "sched_switch"});
        tps.push_back({"sched", "sched_waking"});
        tps.push_back({"sched", "sched_wakeup_new"});
    }
    if (has("irq")) {
        tps.push_back({"irq", "irq_handler_entry"});
        tps.push_back({"irq", "irq_handler_exit"});
    }
    if (has("softirq")) {
        tps.push_back({"irq", "softirq_entry"});
        tps.push_back({"irq", "softirq_exit"});
    }
    if (has("reclaim")) {
        tps.push_back({"vmscan", "mm_vmscan_direct_reclaim_begin"});
        tps.push_back({"vmscan", "mm_vmscan_direct_reclaim_end"});
        tps.push_back({"compaction", "mm_compaction_begin"});
        tps.push_back({"compaction", "mm_compaction_end"});
    }
    if (has("block")) {
        tps.push_back({"block", "block_rq_issue"});
        tps.push_back({"block", "block_rq_complete"});
    }
    return tps;
}

std::vector<std::string> splitComma(std::string_view s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string_view::npos)
            j = s.size();
        if (j > i)
            out.emplace_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

bool knownFeature(const std::string& f)
{
    static const char* known[] = {"sched", "irq", "softirq", "reclaim", "block", "dstate", "pio"};
    return std::any_of(std::begin(known), std::end(known), [&](const char* k) { return f == k; });
}

// ------------------------------------------------------------------ session

struct Session {
    std::string tracefs;
    PerfTracer tracer;
    std::unique_ptr<SchedTracker> tracker;
    PrivProc priv;
    SysctlGuard sysctl;
    std::vector<std::string> features;
    std::vector<TraceEvent> pending, batch;
    std::vector<uint64_t> lost, lostReported;
    SchedTracker::Config cfg;
    bool dstate = false, pio = false;
    int64_t lastPeriodic = 0;
    uint64_t lastEvents = 0;
    int64_t lastCpuNs = 0;

    bool tracing() const { return tracer.isOpen(); }

    bool start(const std::vector<std::string>& feats, std::string& out)
    {
        stop();
        features = feats;
        dstate = std::find(feats.begin(), feats.end(), "dstate") != feats.end();
        pio = std::find(feats.begin(), feats.end(), "pio") != feats.end();
        const auto tps = tracepointsFor(feats);
        if (!tps.empty()) {
            std::string err;
            if (!tracer.open(tracefs, tps, 256, err)) {
                JsonObj("err").str("msg", err).line(out);
                return false;
            }
            if (!err.empty())
                JsonObj("warn").str("msg", err).line(out);
            tracker = std::make_unique<SchedTracker>(tracer, tracer.maxCpu());
            tracker->setConfig(cfg);
            tracer.enable();
        }
        JsonArr caps;
        for (const auto& tp : tracer.tracepoints())
            caps.addStr(tp.sys + ":" + tp.name);
        JsonObj("started").raw("tracepoints", caps.done()).line(out);
        return true;
    }

    void stop()
    {
        tracer.close();
        tracker.reset();
        pending.clear();
        lost.clear();
        lostReported.clear();
        dstate = pio = false;
    }

    // Drain the rings, merge CPUs by time (keeping a short reorder window) and process.
    void drain(int64_t now, std::string& out)
    {
        if (!tracing() || !tracker)
            return;
        batch.clear();
        tracer.drain(batch, lost);
        pending.insert(pending.end(), batch.begin(), batch.end());
        std::sort(pending.begin(), pending.end(), [](const TraceEvent& a, const TraceEvent& b) { return a.timeNs < b.timeNs; });
        // Events newer than (now - 10 ms) may still be racing into another CPU's ring.
        const uint64_t cutoff = uint64_t(now - 10'000'000);
        size_t n = 0;
        while (n < pending.size() && pending[n].timeNs <= cutoff) {
            tracker->process(pending[n], out);
            ++n;
        }
        pending.erase(pending.begin(), pending.begin() + long(n));
        lostReported.resize(lost.size(), 0);
        for (size_t c = 0; c < lost.size(); ++c)
            if (lost[c] != lostReported[c]) {
                JsonObj("lost").num("cpu", int64_t(c)).unum("n", lost[c] - lostReported[c]).line(out);
                lostReported[c] = lost[c];
            }
    }

    void periodic(int64_t now, std::string& out)
    {
        if (now - lastPeriodic < 1'000'000'000)
            return;
        const double dt = lastPeriodic ? double(now - lastPeriodic) / 1e9 : 1.0;
        lastPeriodic = now;
        if (tracker)
            tracker->periodic(now, out);
        if (dstate)
            priv.dstate(now, out);
        if (pio)
            priv.pio(now, out);
        const uint64_t ev = tracker ? tracker->eventCount() : 0;
        const int64_t cpuNs = clockNs(CLOCK_PROCESS_CPUTIME_ID);
        JsonObj("status")
            .dbl("events_ps", double(ev - lastEvents) / dt, 0)
            .dbl("cpu_pct", lastCpuNs ? double(cpuNs - lastCpuNs) / 1e7 / dt : 0.0, 2)
            .line(out);
        lastEvents = ev;
        lastCpuNs = cpuNs;
    }
};

// One command line. Strictly parsed: unknown words are rejected.
bool handleCommand(Session& s, std::string_view line, std::string& out, bool& quit)
{
    Tokens t(line);
    std::string_view cmd;
    if (!t.next(cmd))
        return true;
    if (cmd == "quit") {
        quit = true;
    } else if (cmd == "start") {
        std::string_view feats;
        if (!t.next(feats)) {
            JsonObj("err").str("msg", "start: missing features").line(out);
            return false;
        }
        std::vector<std::string> list = splitComma(feats);
        for (const std::string& f : list)
            if (!knownFeature(f)) {
                JsonObj("err").str("msg", "start: unknown feature").str("feature", f).line(out);
                return false;
            }
        std::string_view opt;
        while (t.next(opt)) {
            int64_t v = 0;
            if (startsWith(opt, "thresh_us=") && parseInt(opt.substr(10), v) && v >= 100 && v <= 10'000'000)
                s.cfg.rqThreshNs = v * 1000;
        }
        s.start(list, out);
    } else if (cmd == "stop") {
        s.stop();
        JsonObj("stopped").line(out);
    } else if (cmd == "window") {
        uint64_t id = 0;
        int64_t cpu = 0, t0 = 0, t1 = 0;
        if (!t.nextInt(id) || !t.nextInt(cpu) || !t.nextInt(t0) || !t.nextInt(t1) || t1 < t0 || t1 - t0 > 10'000'000'000LL) {
            JsonObj("err").str("msg", "window: bad arguments").line(out);
            return false;
        }
        if (s.tracker) {
            // Make sure everything up to t1 has been processed first.
            s.drain(monoNs() + 20'000'000, out);
            s.tracker->window(id, int(cpu), t0, t1, out);
        }
    } else if (cmd == "focus") {
        std::string_view arg;
        int64_t pid = 0;
        if (t.next(arg) && (arg == "none" || (parseInt(arg, pid) && pid > 0))) {
            s.cfg.focusPid = arg == "none" ? 0 : int(pid);
            if (s.tracker)
                s.tracker->setConfig(s.cfg);
        } else {
            JsonObj("err").str("msg", "focus: bad pid").line(out);
        }
    } else if (cmd == "sysctl") {
        std::string_view name, val;
        if (!t.next(name) || !t.next(val) || (val != "on" && val != "off")) {
            JsonObj("err").str("msg", "sysctl: bad arguments").line(out);
            return false;
        }
        std::string err;
        if (!s.sysctl.set(std::string(name), val == "on", err))
            JsonObj("err").str("msg", err).line(out);
        else
            JsonObj("sysctl").str("name", name).str("value", val).line(out);
    } else {
        JsonObj("err").str("msg", "unknown command").line(out);
        return false;
    }
    return true;
}

std::atomic<bool> gStop{false};

// ------------------------------------------------------------------ self-test

int selfTest(Session& s)
{
    std::printf("culprit-helper self-test\n");
    std::string out;
    const std::vector<int> cpus = onlineCpus();
    if (cpus.empty()) {
        std::printf("  FAIL: no online CPUs\n");
        return 1;
    }
    const int cpu = cpus.back();
    s.cfg.rqThreshNs = 200'000;
    if (!s.start({"sched", "irq", "softirq", "reclaim", "block"}, out)) {
        std::printf("  FAIL: %s", out.c_str());
        return 1;
    }
    std::printf("  tracepoints opened: %zu\n", s.tracer.tracepoints().size());
    for (const auto& tp : s.tracer.tracepoints())
        std::printf("    %s:%s (id %llu, %zu fields)\n", tp.sys.c_str(), tp.name.c_str(), (unsigned long long)tp.fmt.id,
                    tp.fmt.fields.size());

    // A hog and a victim pinned to the same CPU; the hog's name contains
    // characters that must not break the JSON protocol.
    std::atomic<bool> stop{false};
    std::atomic<int> hogTid{0}, victimTid{0};
    auto pin = [cpu] {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        sched_setaffinity(0, sizeof set, &set);
    };
    std::thread hog([&] {
        pin();
        pthread_setname_np(pthread_self(), "hog\"\n\x01x");
        hogTid = int(syscall(SYS_gettid));
        while (!stop)
            asm volatile("" ::: "memory");   // spin
    });
    while (!hogTid)
        std::this_thread::yield();
    s.tracker->captureTid = 0;
    std::thread victim([&] {
        pin();
        victimTid = int(syscall(SYS_gettid));
        for (int i = 0; i < 1500 && !stop; ++i) {
            timespec ts{0, 1'000'000};
            nanosleep(&ts, nullptr);
        }
    });
    while (!victimTid)
        std::this_thread::yield();
    s.tracker->captureTid = victimTid;

    const int64_t until = monoNs() + 2'500'000'000LL;
    size_t lines = 0, badLines = 0;
    bool sawEscapedHog = false;
    while (monoNs() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        out.clear();
        s.drain(monoNs(), out);
        // Every protocol line must be exactly one line of JSON-safe text.
        size_t start = 0;
        while (start < out.size()) {
            const size_t nl = out.find('\n', start);
            const std::string_view l(out.data() + start, (nl == std::string::npos ? out.size() : nl) - start);
            ++lines;
            if (l.empty() || l.front() != '{' || l.back() != '}' ||
                std::any_of(l.begin(), l.end(), [](char c) { return (unsigned char)c < 0x20; }))
                ++badLines;
            if (l.find("hog\\\"\\n\\u0001x") != std::string_view::npos)
                sawEscapedHog = true;
            start = nl == std::string::npos ? out.size() : nl + 1;
        }
    }
    stop = true;
    hog.join();
    victim.join();
    out.clear();
    s.drain(monoNs() + 20'000'000, out);
    const uint64_t events = s.tracker->eventCount();

    const auto& cap = s.tracker->captured;
    size_t byHog = 0;
    int64_t worst = 0;
    for (const auto& c : cap) {
        worst = std::max(worst, c.latNs);
        if (c.topBlockerTid == hogTid)
            ++byHog;
    }
    uint64_t lostTotal = 0;
    for (uint64_t l : s.lost)
        lostTotal += l;
    std::printf("  events processed: %llu (%llu lost)\n", (unsigned long long)events, (unsigned long long)lostTotal);
    std::printf("  victim (tid %d) on CPU %d: %zu delayed wakeups, worst %.2f ms\n", victimTid.load(), cpu, cap.size(), double(worst) / 1e6);
    std::printf("  attributed to the hog (tid %d): %zu of %zu\n", hogTid.load(), byHog, cap.size());
    std::printf("  protocol lines: %zu, malformed: %zu, hostile thread name escaped: %s\n", lines, badLines, sawEscapedHog ? "yes" : "not seen");

    // Sysctl round trip.
    std::string cur, err;
    readFirstLine("/proc/sys/kernel/task_delayacct", cur);
    const std::string before = cur;
    const bool setOk = s.sysctl.set("delayacct", before != "1", err);
    s.sysctl.restoreAll();
    readFirstLine("/proc/sys/kernel/task_delayacct", cur);
    const bool restored = cur == before;
    std::printf("  sysctl task_delayacct: toggle %s, restored to %s: %s\n", setOk ? "ok" : err.c_str(), before.c_str(), restored ? "yes" : "NO");
    s.stop();

    const bool pass = !cap.empty() && byHog * 10 >= cap.size() * 8 && badLines == 0 && restored && lostTotal == 0;
    std::printf("  RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    bool selftest = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--selftest")
            selftest = true;
        else if (!startsWith(a, "--proto=")) {
            std::fprintf(stderr, "usage: culprit-helper [--proto=1] [--selftest]\n");
            return 2;
        }
    }
    std::string out;
    if (geteuid() != 0) {
        JsonObj("err").str("msg", "culprit-helper must run as root (it is started through pkexec)").line(out);
        writeAll(1, out);
        return 2;
    }
    // pkexec is setuid, which clears the parent-death signal: set it ourselves,
    // then make sure the parent didn't already go away.
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1 && !selftest)
        return 0;
    SysctlGuard::recoverStale();

    Session s;
    s.tracefs = findTracefs();
    if (s.tracefs.empty()) {
        JsonObj("err").str("msg", "tracefs not available").line(out);
        writeAll(1, out);
        return 1;
    }
    if (selftest)
        return selfTest(s);

    sigset_t mask;
    sigemptyset(&mask);
    for (int sig : {SIGINT, SIGTERM, SIGHUP})
        sigaddset(&mask, sig);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    signal(SIGPIPE, SIG_IGN);
    const int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    const int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    itimerspec its{};
    its.it_interval.tv_nsec = 50'000'000;   // drain every 50 ms
    its.it_value.tv_nsec = 50'000'000;
    timerfd_settime(tfd, 0, &its, nullptr);

    utsname un{};
    uname(&un);
    JsonObj("hello").num("proto", kProto).str("kernel", un.release).num("ncpu", int64_t(onlineCpus().size())).str("tracefs", s.tracefs).line(out);
    if (!writeAll(1, out))
        return 1;

    std::string inbuf;
    bool quit = false;
    while (!quit) {
        pollfd fds[3] = {{0, POLLIN, 0}, {sfd, POLLIN, 0}, {tfd, POLLIN, 0}};
        if (poll(fds, 3, 1000) < 0 && errno != EINTR)
            break;
        out.clear();
        if (fds[1].revents & POLLIN)
            break;   // SIGTERM / SIGINT / SIGHUP
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            char buf[4096];
            const ssize_t n = ::read(0, buf, sizeof buf);
            if (n <= 0)
                break;   // stdin EOF: the GUI went away
            inbuf.append(buf, size_t(n));
            if (inbuf.size() > 65536)
                break;   // nobody sends that much legitimately
            size_t nl;
            while ((nl = inbuf.find('\n')) != std::string::npos) {
                handleCommand(s, std::string_view(inbuf).substr(0, nl), out, quit);
                inbuf.erase(0, nl + 1);
            }
        }
        if (fds[2].revents & POLLIN) {
            uint64_t ticks;
            if (::read(tfd, &ticks, sizeof ticks) < 0) {
            }
        }
        const int64_t now = monoNs();
        s.drain(now, out);
        s.periodic(now, out);
        if (!out.empty() && !writeAll(1, out))
            break;   // the GUI closed the pipe
    }
    s.stop();
    s.sysctl.restoreAll();
    out.clear();
    JsonObj("bye").line(out);
    writeAll(1, out);
    return 0;
}
