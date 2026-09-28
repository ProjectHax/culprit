// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/collect/ProcessCollector.h"

#include "common/fs/File.h"
#include "common/parse/PidParsers.h"
#include "common/parse/Text.h"
#include "common/util/Clock.h"

#include <QtGlobal>

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace culprit {

namespace {

constexpr int kIdleScansBeforeSkip = 3;   // idle this many reads in a row ...
constexpr uint64_t kIdleReadEvery = 3;    // ... then re-read only every third scan

bool isNumeric(const char* s)
{
    if (!*s)
        return false;
    for (; *s; ++s)
        if (*s < '0' || *s > '9')
            return false;
    return true;
}

template <typename F>
void forEachNumericEntry(const std::string& dir, F&& f)
{
    DIR* d = opendir(dir.c_str());
    if (!d)
        return;
    while (dirent* e = readdir(d)) {
        if (isNumeric(e->d_name))
            f(atoi(e->d_name));
    }
    closedir(d);
}

// systemd escapes unit names ("google\x2dchrome"); show them readable.
QString unescapeUnit(std::string_view u)
{
    std::string out;
    out.reserve(u.size());
    for (size_t i = 0; i < u.size(); ++i) {
        if (u[i] == '\\' && i + 3 < u.size() && u[i + 1] == 'x') {
            unsigned v = 0;
            if (parseHex(u.substr(i + 2, 2), v)) {
                out += char(v);
                i += 3;
                continue;
            }
        }
        out += u[i];
    }
    return QString::fromUtf8(out.data(), qsizetype(out.size()));
}

QString wchanOf(const std::string& path)
{
    std::string w;
    if (!readFile(path, w))
        return {};
    std::string_view v = trim(w);
    if (v.empty() || v == "0")   // "0" = hidden (not our process) or running
        return {};
    return QString::fromLatin1(v.data(), qsizetype(v.size()));
}

} // namespace

ProcessCollector::ProcessCollector()
{
    myUid_ = getuid();
    selfPid_ = getpid();
    const long hz = sysconf(_SC_CLK_TCK);
    hz_ = hz > 0 ? double(hz) : 100.0;
    pageSize_ = uint64_t(sysconf(_SC_PAGESIZE));
}

QString ProcessCollector::userName(uint32_t uid)
{
    auto it = users_.find(uid);
    if (it != users_.end())
        return it->second;
    passwd pw{};
    passwd* res = nullptr;
    char buf[1024];
    QString name = QString::number(uid);
    if (getpwuid_r(uid, &pw, buf, sizeof buf, &res) == 0 && res)
        name = QString::fromLocal8Bit(res->pw_name);
    users_[uid] = name;
    return name;
}

void ProcessCollector::loadMeta(int pid, ProcState& st)
{
    // Own buffer: the caller's parsed PidStat still points into buf_.
    std::string buf;
    const std::string base = SysPaths::proc_(std::to_string(pid));
    struct stat sb {};
    if (::stat(base.c_str(), &sb) == 0)
        st.uid = sb.st_uid;
    st.user = userName(st.uid);

    if (readFile(base + "/cmdline", buf) && !buf.empty()) {
        if (buf.size() > 4096)
            buf.resize(4096);
        while (!buf.empty() && buf.back() == '\0')
            buf.pop_back();
        std::replace(buf.begin(), buf.end(), '\0', ' ');
        st.cmdline = QString::fromUtf8(buf.data(), qsizetype(buf.size()));
    }
    if (readFile(base + "/cgroup", buf)) {
        std::string_view path = parseCgroupV2Path(buf);
        st.cgroup = QString::fromUtf8(path.data(), qsizetype(path.size()));
        st.unit = unescapeUnit(cgroupUnitName(path));
    }
    st.metaLoaded = true;
}

void ProcessCollector::sample(Frame& frame, int64_t nowNs, const Config& cfg)
{
    const double dt = lastNs_ > 0 ? double(nowNs - lastNs_) / 1e9 : 0.0;
    const int64_t tStart = monoNs();
    std::vector<ProcSample>& out = frame.procs;
    out.clear();
    out.reserve(procs_.size() + 64);

    std::unordered_set<ProcKey, ProcKeyHash> seen;
    seen.reserve(procs_.size() + 64);

    // ---- tier 1: every process
    const std::string procRoot = SysPaths::instance().proc;
    const bool ioCatchUp = (++tickCount_ % 5) == 0;
    forEachNumericEntry(procRoot, [&](int pid) {
        // Most processes (and nearly all kernel threads) sit idle. Once one has
        // been idle for a few scans it is only re-read every third scan,
        // staggered by pid, and its last sample is repeated in between: each
        // /proc/<pid>/stat read costs ~4 µs of kernel time, ~3 ms per scan.
        if (auto k = pidKeys_.find(pid); k != pidKeys_.end()) {
            auto it = procs_.find(k->second);
            if (it != procs_.end() && it->second.idleScans >= kIdleScansBeforeSkip &&
                (tickCount_ + uint64_t(pid)) % kIdleReadEvery != 0 && !cfg.detailPids.count(pid) &&
                pid != cfg.focusPid) {
                seen.insert(k->second);
                it->second.seenNs = nowNs;
                out.push_back(it->second.last);
                return;
            }
        }

        // Known pid? Re-read its cached stat fd. A stale fd (process exited, pid
        // possibly reused) fails with ESRCH, so it can never mix up processes.
        PidStat ps;
        ProcState* stp = nullptr;
        if (auto k = pidKeys_.find(pid); k != pidKeys_.end()) {
            auto it = procs_.find(k->second);
            if (it != procs_.end() && it->second.statFile.readSingle(buf_) && parsePidStat(buf_, ps) &&
                ps.starttime == k->second.starttime)
                stp = &it->second;
        }
        auto base = [&] { return procRoot + "/" + std::to_string(pid); };
        if (!stp) {
            CachedFile f(base() + "/stat");
            if (!f.readSingle(buf_) || !parsePidStat(buf_, ps))
                return;   // exited meanwhile
            ProcState& fresh = procs_[ProcKey{pid, ps.starttime}];
            if (!fresh.statFile.exists())
                fresh.statFile = std::move(f);
            pidKeys_[pid] = ProcKey{pid, ps.starttime};
            stp = &fresh;
        }
        const ProcKey key{pid, ps.starttime};
        seen.insert(key);
        ProcState& st = *stp;
        if (!st.metaLoaded)
            loadMeta(pid, st);
        if (st.commRaw != ps.comm) {
            st.commRaw.assign(ps.comm);
            st.comm = QString::fromUtf8(ps.comm.data(), qsizetype(ps.comm.size()));
        }

        ProcSample s;
        s.key = key;
        s.ppid = ps.ppid;
        s.comm = st.comm;
        s.cmdline = st.cmdline;
        s.cgroup = st.cgroup;
        s.unit = st.unit;
        s.uid = st.uid;
        s.user = st.user;
        s.state = ps.state;
        s.kernelThread = ps.isKernelThread();
        s.rssBytes = uint64_t(std::max<int64_t>(0, ps.rssPages)) * pageSize_;
        s.threads = int(ps.numThreads);
        s.nice = int(ps.nice);
        s.policy = int(ps.policy);
        s.rtPrio = int(ps.rtPriority);
        s.lastCpu = ps.processor;

        const uint64_t ticks = ps.utime + ps.stime;
        const bool ran = !st.havePrev || ticks != st.cpuTicks;
        // Rates over the time since this process was last read (longer for idle ones).
        const double pdt = st.readNs > 0 ? double(nowNs - st.readNs) / 1e9 : dt;
        if (st.havePrev && pdt > 0) {
            s.cpuPct = ticks >= st.cpuTicks ? double(ticks - st.cpuTicks) / hz_ / pdt * 100.0 : 0.0;
            s.majfltPs = ps.majflt >= st.majflt ? double(ps.majflt - st.majflt) / pdt : 0.0;
            s.minfltPs = ps.minflt >= st.minflt ? double(ps.minflt - st.minflt) / pdt : 0.0;
            if (ps.blkioTicks > 0 || st.blkio > 0)
                s.blkioMsPs = ps.blkioTicks >= st.blkio ? double(ps.blkioTicks - st.blkio) / hz_ * 1000.0 / pdt : 0.0;
        }
        st.idleScans = !ran && (ps.state == 'S' || ps.state == 'I') ? std::min(st.idleScans + 1, 1000) : 0;
        st.readNs = nowNs;
        st.cpuTicks = ticks;
        st.majflt = ps.majflt;
        st.minflt = ps.minflt;
        st.blkio = ps.blkioTicks;

        // Per-process I/O is only readable for our own processes (root helper covers the rest).
        // A process that didn't run can't have issued I/O, so idle ones are only
        // re-read on the periodic catch-up pass.
        if (!s.kernelThread && (st.uid == myUid_ || myUid_ == 0) && (!st.ioTried || st.ioReadable) &&
            (ran || ioCatchUp)) {
            PidIo io;
            if (!st.ioTried)
                st.ioFile.setPath(base() + "/io");
            st.ioTried = true;
            st.ioReadable = st.ioFile.readSingle(buf_) && parsePidIo(buf_, io);
            if (st.ioReadable) {
                if (st.haveIo && pdt > 0) {
                    s.ioReadBps = io.readBytes >= st.ioRead ? double(io.readBytes - st.ioRead) / pdt : 0.0;
                    s.ioWriteBps = io.writeBytes >= st.ioWrite ? double(io.writeBytes - st.ioWrite) / pdt : 0.0;
                }
                st.ioRead = io.readBytes;
                st.ioWrite = io.writeBytes;
                st.haveIo = true;
            }
        } else if (st.ioReadable && st.haveIo) {
            s.ioReadBps = 0;
            s.ioWriteBps = 0;
        }
        if (ps.state == 'D' && st.uid == myUid_)
            s.wchan = wchanOf(base() + "/wchan");

        st.havePrev = true;
        st.seenNs = nowNs;
        st.last = s;
        out.push_back(std::move(s));
    });

    const int64_t tTier1 = monoNs();
    // Forget processes that are gone.
    for (auto it = procs_.begin(); it != procs_.end();) {
        if (!seen.count(it->first)) {
            auto k = pidKeys_.find(it->first.pid);
            if (k != pidKeys_.end() && k->second == it->first)
                pidKeys_.erase(k);
            it = procs_.erase(it);
        } else {
            ++it;
        }
    }

    // ---- tier 2: hot set gets a per-thread scan
    std::vector<size_t> order(out.size());
    for (size_t i = 0; i < out.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return out[a].cpuPct > out[b].cpuPct; });

    std::unordered_set<int> hot;
    std::unordered_set<int> switchSet;
    // Run-queue wait only matters while tasks compete for CPUs. On a quiet system
    // only clearly busy processes get a per-thread scan (apps with 100+ threads
    // idling at 1% would otherwise cost more than everything else); under
    // contention every process using more than 1% of a core does. Hysteresis
    // keeps processes from flapping in and out.
    const bool contended = frame.sys.runDelayTotalPct >= 10;   // ≥ 0.1 tasks waiting on average
    const double enterPct = contended ? 1.0 : 5.0;
    const double exitPct = contended ? 0.3 : 2.0;
    for (size_t i = 0; i < order.size() && int(hot.size()) < cfg.hotCount; ++i) {
        const ProcSample& p = out[order[i]];
        if (p.cpuPct < exitPct)
            break;
        if (p.cpuPct < enterPct && !prevHot_.count(p.key.pid))
            continue;
        hot.insert(p.key.pid);
        // Preemption counts need /proc/<tid>/status for every thread (~10 µs
        // each); they only say something while tasks compete for CPUs.
        if (contended && int(switchSet.size()) < cfg.switchCountTop)
            switchSet.insert(p.key.pid);
    }
    for (const ProcSample& p : out) {
        // A runnable process waits for a CPU only under contention (and Culprit
        // itself is always running while it samples).
        if ((contended && p.state == 'R' && p.key.pid != selfPid_) || p.state == 'D')
            hot.insert(p.key.pid);
    }
    for (int pid : cfg.detailPids) {
        hot.insert(pid);
        switchSet.insert(pid);
    }
    if (cfg.focusPid > 0) {
        hot.insert(cfg.focusPid);
        switchSet.insert(cfg.focusPid);
    }

    std::vector<DStateThread>& dstate = frame.dstate;
    dstate.clear();
    std::unordered_set<int> scanned;
    for (ProcSample& p : out) {
        if (!hot.count(p.key.pid))
            continue;
        auto it = procs_.find(p.key);
        if (it == procs_.end())
            continue;
        scanThreads(p.key.pid, it->second, p, dt, nowNs, switchSet.count(p.key.pid) > 0,
                    cfg.detailPids.count(p.key.pid) > 0, frame.sys.procsBlocked > 0, dstate);
        scanned.insert(p.key.pid);
    }
    // Processes that left the hot set release their per-thread fds.
    for (int pid : prevHot_) {
        if (hot.count(pid))
            continue;
        if (auto k = pidKeys_.find(pid); k != pidKeys_.end())
            if (auto it = procs_.find(k->second); it != procs_.end()) {
                it->second.tasks.clear();
                it->second.lastHotNs = 0;
            }
    }
    prevHot_ = hot;
    const int64_t tTier2 = monoNs();

    // Leaders in D that weren't thread-scanned.
    for (const ProcSample& p : out) {
        if (p.state == 'D' && !scanned.count(p.key.pid)) {
            DStateThread d;
            d.pid = d.tid = p.key.pid;
            d.comm = d.procComm = p.comm;
            d.wchan = p.wchan;
            dstate.push_back(d);
        }
    }

    // ---- tier 3: full D-state thread scan while the kernel reports blocked tasks
    bool fullScan = false;
    if (frame.sys.procsBlocked > 0 && nowNs - lastFullDScanNs_ > 3'000'000'000LL) {
        std::unordered_map<int, const ProcSample*> byPid;
        for (const ProcSample& p : out)
            byPid[p.key.pid] = &p;
        fullDStateScan(byPid, dstate, scanned);
        lastFullDScanNs_ = nowNs;
        fullScan = true;
    }
    finishDState(dstate, nowNs, fullScan || frame.sys.procsBlocked == 0);

    // Hot list for the flight recorder (who ran on a stalled CPU): busiest first.
    // A process that can stall a CPU uses well over 2% of one.
    hotPids_.clear();
    for (size_t i = 0; i < order.size() && hotPids_.size() < 24; ++i) {
        if (out[order[i]].cpuPct < 2.0)
            break;
        hotPids_.push_back(out[order[i]].key.pid);
    }

    static const bool timing = qEnvironmentVariableIsSet("CULPRIT_DEBUG_TIMING");
    if (timing) {
        size_t threads = 0;
        for (const ProcSample& p : out)
            if (p.hot)
                threads += size_t(p.threads);
        std::fprintf(stderr, "procs: %zu processes, tier1 %.2f ms, hot %zu procs/%zu threads (%zu switch) tier2 %.2f ms, tier3 %.2f ms%s", out.size(),
               double(tTier1 - tStart) / 1e6, scanned.size(), threads, switchSet.size(), double(tTier2 - tTier1) / 1e6,
               double(monoNs() - tTier2) / 1e6, fullScan ? " (full D scan)\n" : "\n");
    }
    lastNs_ = nowNs;
}

void ProcessCollector::scanThreads(int pid, ProcState& st, ProcSample& out, double dt, int64_t nowNs,
                                   bool withSwitches, bool withDetails, bool withStates,
                                   std::vector<DStateThread>& dstate)
{
    const std::string taskDir = SysPaths::proc_(std::to_string(pid) + "/task");
    // A delta is only meaningful if this process was also scanned last interval.
    const bool continuous = st.lastHotNs > 0 && (nowNs - st.lastHotNs) < int64_t(dt * 1.5e9) + 200'000'000LL;
    uint64_t waitDelta = 0, nivDelta = 0, nvDelta = 0;
    bool anySwitches = false;
    int dThreads = 0;

    forEachNumericEntry(taskDir, [&](int tid) {
        auto [tit, inserted] = st.tasks.try_emplace(tid);
        TaskState& t = tit->second;
        auto tbase = [&] { return taskDir + "/" + std::to_string(tid); };
        if (inserted)
            t.schedFile.setPath(tbase() + "/schedstat");
        TaskSchedstat ss;
        if (!t.schedFile.readSingle(buf_) || !parseTaskSchedstat(buf_, ss)) {
            st.tasks.erase(tit);
            return;
        }
        PidStat ts;
        bool haveStat = false;
        QString tcomm;   // copied now: ts.comm points into buf_, which is reused below
        if (withDetails || withStates) {
            if (t.statFile.path().empty())
                t.statFile.setPath(tbase() + "/stat");
            haveStat = t.statFile.readSingle(buf_) && parsePidStat(buf_, ts);
            if (haveStat)
                tcomm = QString::fromUtf8(ts.comm.data(), qsizetype(ts.comm.size()));
        }
        const bool haveDelta = continuous && !inserted;
        const uint64_t wd = haveDelta && ss.waitNs >= t.waitNs ? ss.waitNs - t.waitNs : 0;
        waitDelta += wd;

        ThreadSample th;
        if (haveStat) {
            const uint64_t ticks = ts.utime + ts.stime;
            if (haveDelta && dt > 0 && ticks >= t.cpuTicks)
                th.cpuPct = double(ticks - t.cpuTicks) / hz_ / dt * 100.0;
            t.cpuTicks = ticks;
            if (ts.state == 'D') {
                ++dThreads;
                DStateThread d;
                d.pid = pid;
                d.tid = tid;
                d.comm = tcomm;
                d.procComm = out.comm;
                if (st.uid == myUid_)
                    d.wchan = wchanOf(tbase() + "/wchan");
                dstate.push_back(d);
            }
        }
        if (withSwitches) {
            PidStatus status;
            if (t.statusFile.path().empty())
                t.statusFile.setPath(tbase() + "/status");
            if (t.statusFile.readSingle(buf_) && parsePidStatus(buf_, status)) {
                if (haveDelta && t.haveSwitches) {
                    nivDelta += status.nonvolCtxsw >= t.nivcsw ? status.nonvolCtxsw - t.nivcsw : 0;
                    nvDelta += status.volCtxsw >= t.nvcsw ? status.volCtxsw - t.nvcsw : 0;
                    anySwitches = true;
                }
                if (withDetails && haveDelta && t.haveSwitches && dt > 0)
                    th.nivcswPs = double(status.nonvolCtxsw - std::min(status.nonvolCtxsw, t.nivcsw)) / dt;
                t.nivcsw = status.nonvolCtxsw;
                t.nvcsw = status.volCtxsw;
                t.haveSwitches = true;
            }
        }
        if (withDetails && haveStat) {
            th.tid = tid;
            th.comm = tcomm;
            th.state = ts.state;
            th.runDelayMsPs = haveDelta && dt > 0 ? double(wd) / 1e6 / dt : -1;
            th.lastCpu = ts.processor;
            th.nice = int(ts.nice);
            th.policy = int(ts.policy);
            th.rtPrio = int(ts.rtPriority);
            out.threadDetails.push_back(std::move(th));
        }
        t.waitNs = ss.waitNs;
        t.runNs = ss.runNs;
        t.seenNs = nowNs;
    });

    // Drop exited threads.
    for (auto it = st.tasks.begin(); it != st.tasks.end();) {
        if (it->second.seenNs != nowNs)
            it = st.tasks.erase(it);
        else
            ++it;
    }

    out.hot = true;
    out.dThreads = dThreads;
    if (continuous && dt > 0) {
        out.runDelayMsPs = double(waitDelta) / 1e6 / dt;
        if (anySwitches) {
            out.nivcswPs = double(nivDelta) / dt;
            out.nvcswPs = double(nvDelta) / dt;
        }
    }
    st.lastHotNs = nowNs;
}

void ProcessCollector::fullDStateScan(const std::unordered_map<int, const ProcSample*>& byPid,
                                      std::vector<DStateThread>& dstate, const std::unordered_set<int>& alreadyScanned)
{
    const std::string procRoot = SysPaths::instance().proc;
    for (const auto& [pid, p] : byPid) {
        if (alreadyScanned.count(pid) || p->threads <= 1)
            continue;   // single-threaded processes were covered by the leader check
        const std::string taskDir = procRoot + "/" + std::to_string(pid) + "/task";
        forEachNumericEntry(taskDir, [&](int tid) {
            if (tid == pid)
                return;
            PidStat ts;
            if (!readFile(taskDir + "/" + std::to_string(tid) + "/stat", buf_) || !parsePidStat(buf_, ts) || ts.state != 'D')
                return;
            DStateThread d;
            d.pid = pid;
            d.tid = tid;
            d.comm = QString::fromUtf8(ts.comm.data(), qsizetype(ts.comm.size()));
            d.procComm = p->comm;
            if (p->uid == myUid_)
                d.wchan = wchanOf(taskDir + "/" + std::to_string(tid) + "/wchan");
            dstate.push_back(d);
        });
    }
}

void ProcessCollector::finishDState(std::vector<DStateThread>& dstate, int64_t nowNs, bool authoritative)
{
    std::unordered_set<int> current;
    for (DStateThread& d : dstate) {
        current.insert(d.tid);
        auto [it, inserted] = dSince_.try_emplace(d.tid, nowNs);
        d.ageSec = double(nowNs - it->second) / 1e9;
    }
    // Only forget tids when this sample saw every thread (otherwise a D thread
    // in a non-scanned process would have its age reset).
    if (authoritative) {
        for (auto it = dSince_.begin(); it != dSince_.end();) {
            if (!current.count(it->first))
                it = dSince_.erase(it);
            else
                ++it;
        }
    }
    std::sort(dstate.begin(), dstate.end(), [](const DStateThread& a, const DStateThread& b) { return a.ageSec > b.ageSec; });
}

} // namespace culprit
