// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/stutter/LatencyProbe.h"

#include "common/parse/Text.h"
#include "common/util/Clock.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>

#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace culprit {

namespace {

// Second field of /proc/thread-self/schedstat: total ns spent runnable-but-waiting.
bool readRunDelay(int fd, uint64_t& out)
{
    char buf[128];
    const ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
    if (n <= 0)
        return false;
    Tokens t(std::string_view(buf, size_t(n)));
    uint64_t run = 0;
    return t.nextInt(run) && t.nextInt(out);
}

} // namespace

ProbeSet::~ProbeSet() { stop(); }

bool ProbeSet::start(const Config& cfg, int ncpu, QString* error)
{
    stop();
    if (cfg.mode == ProbeMode::Off)
        return true;
    stop_ = false;
    std::vector<int> cpus;
    if (cfg.mode == ProbeMode::PerCpu) {
        cpu_set_t allowed;
        CPU_ZERO(&allowed);
        sched_getaffinity(0, sizeof allowed, &allowed);
        for (int c = 0; c < ncpu; ++c)
            if (CPU_ISSET(c, &allowed))
                cpus.push_back(c);
    } else {
        cpus.assign(size_t(std::max(1, cfg.count)), -1);
    }
    const int64_t periodNs = int64_t(std::max(100, cfg.periodUs)) * 1000;
    for (size_t i = 0; i < cpus.size(); ++i) {
        auto p = std::make_unique<Probe>();
        p->cpu = cpus[i];
        p->id = int(i);
        Probe* raw = p.get();
        probes_.push_back(std::move(p));
        raw->thread = std::thread([this, raw, periodNs] { run(raw, periodNs); });
    }
    // Wait until every probe has published its tid.
    for (auto& p : probes_)
        while (p->tid.load() == 0)
            std::this_thread::yield();
    if (error)
        error->clear();
    return true;
}

void ProbeSet::stop()
{
    stop_ = true;
    for (auto& p : probes_)
        if (p->thread.joinable())
            p->thread.join();
    probes_.clear();
}

std::vector<pid_t> ProbeSet::tids() const
{
    std::vector<pid_t> out;
    for (const auto& p : probes_)
        out.push_back(p->tid.load());
    return out;
}

uint64_t ProbeSet::dropped() const
{
    uint64_t n = 0;
    for (const auto& p : probes_)
        n += p->dropped.load();
    return n;
}

void ProbeSet::run(Probe* p, int64_t periodNs)
{
    pthread_setname_np(pthread_self(), "culprit-probe");
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);   // wake exactly on the deadline
    if (p->cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(p->cpu, &set);
        sched_setaffinity(0, sizeof set, &set);
    }
    // /proc/thread-self is bound to this thread when opened.
    const int fd = open("/proc/thread-self/schedstat", O_RDONLY | O_CLOEXEC);
    p->tid = pid_t(syscall(SYS_gettid));

    uint64_t lastWait = 0;
    if (fd >= 0)
        readRunDelay(fd, lastWait);
    int64_t suspendOffset = bootNs() - monoNs();
    int64_t next = monoNs() + periodNs;
    while (!stop_.load(std::memory_order_relaxed)) {
        const timespec ts = nsToTimespec(next);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
        }
        const int64_t now = monoNs();
        uint64_t wait = lastWait;
        if (fd >= 0)
            readRunDelay(fd, wait);

        ProbeSample s;
        s.expectedNs = next;
        s.actualNs = now;
        s.runDelayNs = int64_t(wait - lastWait);
        s.cpu = sched_getcpu();
        s.probe = int16_t(p->id);
        const int64_t offset = bootNs() - now;
        if (offset - suspendOffset > 50'000'000) {   // clock jumped: we were suspended
            s.flags |= ProbeSample::kSuspended;
            suspendOffset = offset;
        }
        if (!p->ring.push(s))
            p->dropped.fetch_add(1, std::memory_order_relaxed);
        lastWait = wait;

        next += periodNs;
        if (now > next)   // we overran: don't burst to catch up
            next = now + periodNs;
    }
    if (fd >= 0)
        close(fd);
}

bool makeThreadRealtime(pid_t tid, int priority, QString* error)
{
    // rtkit only grants RT to processes that bound their RT CPU time.
    rlimit rl{};
    getrlimit(RLIMIT_RTTIME, &rl);
    if (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > 200000) {
        rl.rlim_cur = rl.rlim_max = 200000;   // µs of RT runtime without sleeping
        setrlimit(RLIMIT_RTTIME, &rl);
    }
    // Directly first (works as root or with RLIMIT_RTPRIO), then via rtkit.
    sched_param sp{};
    sp.sched_priority = priority;
    if (sched_setscheduler(tid, SCHED_FIFO | SCHED_RESET_ON_FORK, &sp) == 0)
        return true;

    QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.RealtimeKit1"),
                                                      QStringLiteral("/org/freedesktop/RealtimeKit1"),
                                                      QStringLiteral("org.freedesktop.RealtimeKit1"),
                                                      QStringLiteral("MakeThreadRealtime"));
    msg << quint64(tid) << quint32(priority);
    const QDBusMessage reply = QDBusConnection::systemBus().call(msg, QDBus::Block, 2000);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (error)
            *error = reply.errorMessage();
        return false;
    }
    return true;
}

} // namespace culprit
