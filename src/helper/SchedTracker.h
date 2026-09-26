// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "helper/PerfTracer.h"

#include <array>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace culprit {

// Turns scheduler / IRQ / reclaim / block tracepoints into latency facts:
//  * run-queue latency per task (woken or preempted -> actually running),
//    with the tasks/IRQs/softirqs that occupied that CPU meanwhile ("blockers")
//  * long hard-IRQ and softirq handlers
//  * direct reclaim / compaction stalls, slow block requests
//  * a 5 s per-CPU occupancy timeline to answer "what ran on CPU c between t0 and t1"
class SchedTracker {
public:
    struct Config {
        int64_t rqThreshNs = 2'000'000;
        int64_t irqThreshNs = 500'000;
        int64_t softirqThreshNs = 1'000'000;
        int64_t reclaimThreshNs = 1'000'000;
        int64_t blkThreshNs = 50'000'000;
        int focusPid = 0;
        int maxRqEventsPerSec = 100;
    };
    // For the self-test: every run-queue wait of `captureTid`.
    struct Captured {
        int64_t latNs;
        int32_t topBlockerTid;
        int64_t topBlockerNs;
    };

    SchedTracker(const PerfTracer& tracer, int maxCpu);

    void setConfig(const Config& c) { cfg_ = c; }
    const Config& config() const { return cfg_; }
    void process(const TraceEvent& ev, std::string& out);
    void periodic(int64_t nowNs, std::string& out);
    void window(uint64_t id, int cpu, int64_t t0, int64_t t1, std::string& out) const;
    void prune(int64_t nowNs);
    uint64_t eventCount() const { return events_; }

    int captureTid = 0;
    std::vector<Captured> captured;

private:
    enum Kind : uint8_t { KTask, KIrq, KSoftirq, KIdle };
    struct Slice {
        int64_t start, end;
        uint8_t kind;
        int32_t id;    // tid / irq number / softirq vector
        int32_t pid;   // tgid for tasks
        char name[16];
    };
    struct CpuState {
        int32_t curTid = -1, curPid = 0;
        char curComm[16] = {};
        int64_t curStart = 0;
        bool inIrq = false, inSoft = false;
        int32_t irq = 0, vec = 0;
        char irqName[16] = {};
        int64_t irqStart = 0, softStart = 0;
        std::deque<Slice> timeline;
    };
    struct Wait {
        int64_t start;
        uint8_t why;   // 0 wake, 1 preempt
    };
    struct Blocker {
        uint8_t kind;
        int32_t id, pid;
        std::string name;
        int64_t ns;
    };
    struct WaiterAgg {
        int32_t pid = 0;
        char comm[16] = {};
        uint32_t n = 0;
        uint64_t sumNs = 0, maxNs = 0;
    };
    struct BlkReq {
        int64_t start;
        uint64_t bytes;
        char rwbs[8];
        char comm[16];
    };

    void onSwitch(const TraceEvent& ev, std::string& out);
    void onWaking(const TraceEvent& ev);
    void onRunLatency(int32_t tid, int cpu, int64_t start, int64_t end, uint8_t why, std::string& out);
    std::vector<Blocker> blockers(int cpu, int64_t t0, int64_t t1, size_t max) const;
    void pushSlice(CpuState& cs, const Slice& s);
    int32_t tgidOf(int32_t tid) const;
    std::string commOf(int32_t tid) const;
    static int log2Bucket(int64_t ns);

    Config cfg_;
    uint64_t events_ = 0;
    int tpSwitch_ = -1, tpWaking_ = -1, tpWakeupNew_ = -1, tpIrqEntry_ = -1, tpIrqExit_ = -1, tpSoftEntry_ = -1,
        tpSoftExit_ = -1, tpReclaimBegin_ = -1, tpReclaimEnd_ = -1, tpCompactBegin_ = -1, tpCompactEnd_ = -1,
        tpBlkIssue_ = -1, tpBlkComplete_ = -1;
    const TraceField *swPrevComm_ = nullptr, *swPrevPid_ = nullptr, *swPrevState_ = nullptr, *swNextComm_ = nullptr,
                     *swNextPid_ = nullptr;
    const TraceField *wkComm_ = nullptr, *wkPid_ = nullptr, *wnComm_ = nullptr, *wnPid_ = nullptr;
    const TraceField *irqNum_ = nullptr, *irqName_ = nullptr, *softVec_ = nullptr, *softExitVec_ = nullptr;
    const TraceField *biDev_ = nullptr, *biSector_ = nullptr, *biBytes_ = nullptr, *biRwbs_ = nullptr, *biComm_ = nullptr;
    const TraceField *bcDev_ = nullptr, *bcSector_ = nullptr;

    std::vector<CpuState> cpus_;
    std::unordered_map<int32_t, Wait> waits_;
    mutable std::unordered_map<int32_t, int32_t> tgid_;
    std::unordered_map<int32_t, std::array<char, 16>> comm_;
    std::unordered_map<int32_t, int64_t> reclaimStart_, compactStart_;
    std::unordered_map<uint64_t, BlkReq> blk_;
    std::unordered_map<int32_t, WaiterAgg> waiters_;
    std::array<uint64_t, 24> rqHist_{}, irqHist_{}, softHist_{};
    int rqEmitted_ = 0, irqEmitted_ = 0;
    uint64_t rqSuppressed_ = 0;
    int64_t lastPrune_ = 0;
};

} // namespace culprit
