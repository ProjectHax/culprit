// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "helper/SchedTracker.h"

#include "common/fs/File.h"
#include "common/parse/PidParsers.h"
#include "common/util/JsonWriter.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace culprit {

namespace {

constexpr int64_t kTimelineNs = 5'000'000'000;
// prev_state: 0 = still runnable (yield / voluntary resched); TASK_REPORT_MAX
// (0x100 on 6.x) = preempted. Anything else is a sleep state.
constexpr int64_t kPreemptedState = 0x100;

const char* softirqNames[] = {"HI", "TIMER", "NET_TX", "NET_RX", "BLOCK", "IRQ_POLL", "TASKLET", "SCHED", "HRTIMER", "RCU"};

std::string_view vecName(int v) { return v >= 0 && v < 10 ? softirqNames[v] : "?"; }

void copyName(char (&dst)[16], std::string_view src)
{
    const size_t n = std::min<size_t>(src.size(), 15);
    std::memcpy(dst, src.data(), n);
    dst[n] = 0;
}

} // namespace

SchedTracker::SchedTracker(const PerfTracer& t, int maxCpu) : cpus_(size_t(std::max(1, maxCpu)))
{
    auto fmt = [&](const char* sys, const char* name) -> std::pair<int, const TraceFormat*> {
        const int i = t.indexOf(sys, name);
        return {i, i >= 0 ? &t.tracepoints()[size_t(i)].fmt : nullptr};
    };
    if (auto [i, f] = fmt("sched", "sched_switch"); f) {
        tpSwitch_ = i;
        swPrevComm_ = f->find("prev_comm");
        swPrevPid_ = f->find("prev_pid");
        swPrevState_ = f->find("prev_state");
        swNextComm_ = f->find("next_comm");
        swNextPid_ = f->find("next_pid");
    }
    if (auto [i, f] = fmt("sched", "sched_waking"); f) {
        tpWaking_ = i;
        wkComm_ = f->find("comm");
        wkPid_ = f->find("pid");
    }
    if (auto [i, f] = fmt("sched", "sched_wakeup_new"); f) {
        tpWakeupNew_ = i;
        wnComm_ = f->find("comm");
        wnPid_ = f->find("pid");
    }
    if (auto [i, f] = fmt("irq", "irq_handler_entry"); f) {
        tpIrqEntry_ = i;
        irqNum_ = f->find("irq");
        irqName_ = f->find("name");
    }
    tpIrqExit_ = fmt("irq", "irq_handler_exit").first;
    if (auto [i, f] = fmt("irq", "softirq_entry"); f) {
        tpSoftEntry_ = i;
        softVec_ = f->find("vec");
    }
    if (auto [i, f] = fmt("irq", "softirq_exit"); f) {
        tpSoftExit_ = i;
        softExitVec_ = f->find("vec");
    }
    tpReclaimBegin_ = fmt("vmscan", "mm_vmscan_direct_reclaim_begin").first;
    tpReclaimEnd_ = fmt("vmscan", "mm_vmscan_direct_reclaim_end").first;
    tpCompactBegin_ = fmt("compaction", "mm_compaction_begin").first;
    tpCompactEnd_ = fmt("compaction", "mm_compaction_end").first;
    if (auto [i, f] = fmt("block", "block_rq_issue"); f) {
        tpBlkIssue_ = i;
        biDev_ = f->find("dev");
        biSector_ = f->find("sector");
        biBytes_ = f->find("bytes");
        biRwbs_ = f->find("rwbs");
        biComm_ = f->find("comm");
    }
    if (auto [i, f] = fmt("block", "block_rq_complete"); f) {
        tpBlkComplete_ = i;
        bcDev_ = f->find("dev");
        bcSector_ = f->find("sector");
    }
}

int SchedTracker::log2Bucket(int64_t ns)
{
    int64_t us = ns / 1000;
    int b = 0;
    while (us > 1 && b < 23) {
        us >>= 1;
        ++b;
    }
    return b;
}

int32_t SchedTracker::tgidOf(int32_t tid) const
{
    if (tid <= 0)
        return 0;
    auto it = tgid_.find(tid);
    if (it != tgid_.end())
        return it->second;
    // Not seen switching out yet: ask /proc (only on reporting paths, so rare).
    std::string status;
    PidStatus ps;
    int32_t tgid = 0;
    if (readFile("/proc/" + std::to_string(tid) + "/status", status) && parsePidStatus(status, ps))
        tgid = ps.tgid;
    tgid_[tid] = tgid;
    return tgid;
}

std::string SchedTracker::commOf(int32_t tid) const
{
    auto it = comm_.find(tid);
    if (it == comm_.end())
        return {};
    return std::string(it->second.data(), strnlen(it->second.data(), 16));
}

void SchedTracker::pushSlice(CpuState& cs, const Slice& s)
{
    if (s.end > s.start)
        cs.timeline.push_back(s);
}

void SchedTracker::process(const TraceEvent& ev, std::string& out)
{
    ++events_;
    if (ev.cpu < 0 || size_t(ev.cpu) >= cpus_.size())
        return;
    CpuState& cs = cpus_[size_t(ev.cpu)];
    const int64_t t = int64_t(ev.timeNs);
    const uint8_t* raw = ev.raw;
    const uint32_t len = ev.rawLen;

    if (ev.tp == tpSwitch_) {
        onSwitch(ev, out);
    } else if (ev.tp == tpWaking_ || ev.tp == tpWakeupNew_) {
        onWaking(ev);
    } else if (ev.tp == tpIrqEntry_) {
        cs.inIrq = true;
        cs.irq = int32_t(traceS(raw, len, irqNum_));
        copyName(cs.irqName, traceStr(raw, len, irqName_));
        cs.irqStart = t;
    } else if (ev.tp == tpIrqExit_) {
        if (cs.inIrq) {
            const int64_t d = t - cs.irqStart;
            irqHist_[size_t(log2Bucket(d))]++;
            Slice s{cs.irqStart, t, KIrq, cs.irq, 0, {}};
            std::memcpy(s.name, cs.irqName, 16);
            pushSlice(cs, s);
            if (d >= cfg_.irqThreshNs && irqEmitted_ < 50) {
                ++irqEmitted_;
                JsonObj("irq").num("ts", cs.irqStart).num("cpu", ev.cpu).num("irq", cs.irq).str("name", cs.irqName).dbl("us", double(d) / 1e3).line(out);
            }
            cs.inIrq = false;
        }
    } else if (ev.tp == tpSoftEntry_) {
        cs.inSoft = true;
        cs.vec = int32_t(traceU(raw, len, softVec_));
        cs.softStart = t;
    } else if (ev.tp == tpSoftExit_) {
        if (cs.inSoft) {
            const int64_t d = t - cs.softStart;
            softHist_[size_t(log2Bucket(d))]++;
            Slice s{cs.softStart, t, KSoftirq, cs.vec, 0, {}};
            copyName(s.name, vecName(cs.vec));
            pushSlice(cs, s);
            if (d >= cfg_.softirqThreshNs && irqEmitted_ < 50) {
                ++irqEmitted_;
                JsonObj("softirq").num("ts", cs.softStart).num("cpu", ev.cpu).str("vec", vecName(cs.vec)).dbl("us", double(d) / 1e3).line(out);
            }
            cs.inSoft = false;
        }
    } else if (ev.tp == tpReclaimBegin_) {
        reclaimStart_[ev.tid] = t;
    } else if (ev.tp == tpReclaimEnd_ || ev.tp == tpCompactEnd_) {
        auto& m = ev.tp == tpReclaimEnd_ ? reclaimStart_ : compactStart_;
        auto it = m.find(ev.tid);
        if (it != m.end()) {
            const int64_t d = t - it->second;
            if (d >= cfg_.reclaimThreshNs)
                JsonObj(ev.tp == tpReclaimEnd_ ? "reclaim" : "compact")
                    .num("ts", it->second)
                    .num("cpu", ev.cpu)
                    .num("pid", ev.pid)
                    .num("tid", ev.tid)
                    .str("comm", commOf(ev.tid))
                    .dbl("us", double(d) / 1e3)
                    .line(out);
            m.erase(it);
        }
    } else if (ev.tp == tpCompactBegin_) {
        compactStart_[ev.tid] = t;
    } else if (ev.tp == tpBlkIssue_) {
        if (blk_.size() > 200000)
            blk_.clear();
        const uint64_t key = (traceU(raw, len, biDev_) << 44) ^ traceU(raw, len, biSector_);
        BlkReq r{t, traceU(raw, len, biBytes_), {}, {}};
        const std::string_view rwbs = traceStr(raw, len, biRwbs_);
        std::memcpy(r.rwbs, rwbs.data(), std::min<size_t>(rwbs.size(), 7));
        copyName(r.comm, traceStr(raw, len, biComm_));
        blk_[key] = r;
    } else if (ev.tp == tpBlkComplete_) {
        const uint64_t dev = traceU(raw, len, bcDev_);
        const uint64_t key = (dev << 44) ^ traceU(raw, len, bcSector_);
        auto it = blk_.find(key);
        if (it != blk_.end()) {
            const int64_t d = t - it->second.start;
            if (d >= cfg_.blkThreshNs) {
                char devs[24];
                std::snprintf(devs, sizeof devs, "%u:%u", unsigned(dev >> 20), unsigned(dev & 0xfffff));
                JsonObj("blk")
                    .num("ts", it->second.start)
                    .str("dev", devs)
                    .str("rwbs", std::string_view(it->second.rwbs, strnlen(it->second.rwbs, 8)))
                    .unum("bytes", it->second.bytes)
                    .str("comm", std::string_view(it->second.comm, strnlen(it->second.comm, 16)))
                    .dbl("us", double(d) / 1e3)
                    .line(out);
            }
            blk_.erase(it);
        }
    }
}

void SchedTracker::onWaking(const TraceEvent& ev)
{
    const bool isNew = ev.tp == tpWakeupNew_;
    const int32_t tid = int32_t(traceS(ev.raw, ev.rawLen, isNew ? wnPid_ : wkPid_));
    if (tid <= 0)
        return;
    std::array<char, 16> c{};
    const std::string_view name = traceStr(ev.raw, ev.rawLen, isNew ? wnComm_ : wkComm_);
    std::memcpy(c.data(), name.data(), std::min<size_t>(name.size(), 15));
    comm_[tid] = c;
    waits_.try_emplace(tid, Wait{int64_t(ev.timeNs), 0});
}

void SchedTracker::onSwitch(const TraceEvent& ev, std::string& out)
{
    CpuState& cs = cpus_[size_t(ev.cpu)];
    const int64_t t = int64_t(ev.timeNs);
    const int32_t prev = int32_t(traceS(ev.raw, ev.rawLen, swPrevPid_));
    const int32_t next = int32_t(traceS(ev.raw, ev.rawLen, swNextPid_));
    const int64_t prevState = traceS(ev.raw, ev.rawLen, swPrevState_);

    // The sample's pid/tid belong to the task switching out.
    if (prev > 0 && ev.tid == prev)
        tgid_[prev] = ev.pid;
    std::array<char, 16> pc{}, nc{};
    const std::string_view pn = traceStr(ev.raw, ev.rawLen, swPrevComm_);
    const std::string_view nn = traceStr(ev.raw, ev.rawLen, swNextComm_);
    std::memcpy(pc.data(), pn.data(), std::min<size_t>(pn.size(), 15));
    std::memcpy(nc.data(), nn.data(), std::min<size_t>(nn.size(), 15));
    if (prev > 0)
        comm_[prev] = pc;
    if (next > 0)
        comm_[next] = nc;

    // Close the outgoing task's slice.
    if (cs.curTid >= 0 && cs.curStart > 0) {
        Slice s{cs.curStart, t, cs.curTid == 0 ? uint8_t(KIdle) : uint8_t(KTask), cs.curTid, cs.curPid, {}};
        std::memcpy(s.name, cs.curComm, 16);
        pushSlice(cs, s);
    }
    // Still runnable (preempted / yielded): it now waits for a CPU.
    if (prev > 0 && (prevState == 0 || (prevState & kPreemptedState)))
        waits_.try_emplace(prev, Wait{t, 1});

    cs.curTid = next;
    cs.curPid = next > 0 ? tgidOf(next) : 0;
    std::memcpy(cs.curComm, nc.data(), 16);
    cs.curComm[15] = 0;
    cs.curStart = t;

    if (next > 0) {
        auto it = waits_.find(next);
        if (it != waits_.end()) {
            const Wait w = it->second;
            waits_.erase(it);
            onRunLatency(next, ev.cpu, w.start, t, w.why, out);
        }
    }
}

std::vector<SchedTracker::Blocker> SchedTracker::blockers(int cpu, int64_t t0, int64_t t1, size_t max) const
{
    std::map<std::pair<uint8_t, int32_t>, Blocker> agg;
    auto scan = [&](const CpuState& cs) {
        for (auto it = cs.timeline.rbegin(); it != cs.timeline.rend(); ++it) {
            const Slice& s = *it;
            if (s.end <= t0)
                break;   // timeline is (mostly) time ordered
            const int64_t ov = std::min(s.end, t1) - std::max(s.start, t0);
            if (ov <= 0)
                continue;
            Blocker& b = agg[{s.kind, s.id}];
            if (b.ns == 0) {
                b.kind = s.kind;
                b.id = s.id;
                b.pid = s.pid ? s.pid : tgidOf(s.id);
                b.name.assign(s.name, strnlen(s.name, 16));
            }
            b.ns += ov;
        }
    };
    if (cpu >= 0 && size_t(cpu) < cpus_.size())
        scan(cpus_[size_t(cpu)]);
    else
        for (const CpuState& cs : cpus_)
            scan(cs);
    std::vector<Blocker> v;
    for (auto& [k, b] : agg)
        v.push_back(std::move(b));
    std::sort(v.begin(), v.end(), [](const Blocker& a, const Blocker& b) { return a.ns > b.ns; });
    if (v.size() > max)
        v.resize(max);
    return v;
}

namespace {
const char* kindName(uint8_t k)
{
    switch (k) {
    case 0: return "task";
    case 1: return "irq";
    case 2: return "softirq";
    default: return "idle";
    }
}
} // namespace

void SchedTracker::onRunLatency(int32_t tid, int cpu, int64_t start, int64_t end, uint8_t why, std::string& out)
{
    const int64_t lat = end - start;
    if (lat <= 0 || lat > 10'000'000'000LL)
        return;
    rqHist_[size_t(log2Bucket(lat))]++;
    const int32_t pid = tgidOf(tid);
    WaiterAgg& w = waiters_[tid];
    w.pid = pid;
    const std::string comm = commOf(tid);
    std::memcpy(w.comm, comm.data(), std::min<size_t>(comm.size(), 15));
    w.n++;
    w.sumNs += uint64_t(lat);
    w.maxNs = std::max<uint64_t>(w.maxNs, uint64_t(lat));

    if (captureTid && tid == captureTid) {
        const auto b = blockers(cpu, start, end, 1);
        captured.push_back({lat, b.empty() ? -1 : b.front().id, b.empty() ? 0 : b.front().ns});
    }

    const bool focus = cfg_.focusPid > 0 && pid == cfg_.focusPid && lat >= 1'000'000;
    if (lat < cfg_.rqThreshNs && !focus)
        return;
    if (rqEmitted_ >= cfg_.maxRqEventsPerSec) {
        ++rqSuppressed_;
        return;
    }
    ++rqEmitted_;
    JsonArr arr;
    for (const Blocker& b : blockers(cpu, start, end, 6)) {
        JsonObj o;
        o.str("k", kindName(b.kind));
        if (b.kind == KTask || b.kind == KIdle)
            o.num("tid", b.id).num("pid", b.pid);
        o.str("name", b.name).dbl("us", double(b.ns) / 1e3);
        arr.add(o.done());
    }
    JsonObj("rqlat")
        .num("ts", start)
        .num("cpu", cpu)
        .num("tid", tid)
        .num("pid", pid)
        .str("comm", comm)
        .dbl("us", double(lat) / 1e3)
        .str("why", why ? "preempt" : "wake")
        .num("focus", focus ? 1 : 0)
        .raw("blockers", arr.done())
        .line(out);
}

void SchedTracker::window(uint64_t id, int cpu, int64_t t0, int64_t t1, std::string& out) const
{
    JsonArr arr;
    for (const Blocker& b : blockers(cpu, t0, t1, 12)) {
        JsonObj o;
        o.str("k", kindName(b.kind));
        if (b.kind == KTask || b.kind == KIdle)
            o.num("tid", b.id).num("pid", b.pid);
        o.str("name", b.name).dbl("us", double(b.ns) / 1e3);
        arr.add(o.done());
    }
    JsonObj("window").unum("id", id).num("cpu", cpu).num("t0", t0).num("t1", t1).raw("slices", arr.done()).line(out);
}

void SchedTracker::periodic(int64_t nowNs, std::string& out)
{
    // Top run-queue waiters of the last second.
    std::vector<std::pair<int32_t, const WaiterAgg*>> v;
    for (const auto& [tid, w] : waiters_)
        if (w.sumNs >= 1'000'000)
            v.emplace_back(tid, &w);
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second->sumNs > b.second->sumNs; });
    JsonArr rows;
    for (size_t i = 0; i < v.size() && i < 15; ++i) {
        const WaiterAgg& w = *v[i].second;
        rows.add(JsonObj()
                     .num("tid", v[i].first)
                     .num("pid", w.pid)
                     .str("comm", std::string_view(w.comm, strnlen(w.comm, 16)))
                     .unum("n", w.n)
                     .dbl("sum_us", double(w.sumNs) / 1e3, 0)
                     .dbl("max_us", double(w.maxNs) / 1e3, 0)
                     .done());
    }
    JsonObj("rqtop").raw("rows", rows.done()).unum("suppressed", rqSuppressed_).line(out);
    waiters_.clear();

    auto hist = [](const std::array<uint64_t, 24>& h) {
        JsonArr a;
        for (uint64_t x : h)
            a.addNum(double(x));
        return a.done();
    };
    JsonObj("hist").raw("rq", hist(rqHist_)).raw("irq", hist(irqHist_)).raw("softirq", hist(softHist_)).line(out);
    rqHist_.fill(0);
    irqHist_.fill(0);
    softHist_.fill(0);
    rqEmitted_ = 0;
    irqEmitted_ = 0;
    rqSuppressed_ = 0;
    prune(nowNs);
}

void SchedTracker::prune(int64_t nowNs)
{
    for (CpuState& cs : cpus_)
        while (!cs.timeline.empty() && cs.timeline.front().end < nowNs - kTimelineNs)
            cs.timeline.pop_front();
    if (nowNs - lastPrune_ > 10'000'000'000LL) {
        lastPrune_ = nowNs;
        for (auto it = waits_.begin(); it != waits_.end();)
            it = nowNs - it->second.start > 10'000'000'000LL ? waits_.erase(it) : std::next(it);
        for (auto* m : {&reclaimStart_, &compactStart_})
            for (auto it = m->begin(); it != m->end();)
                it = nowNs - it->second > 60'000'000'000LL ? m->erase(it) : std::next(it);
        if (tgid_.size() > 200000)
            tgid_.clear();
        if (comm_.size() > 200000)
            comm_.clear();
    }
}

} // namespace culprit
