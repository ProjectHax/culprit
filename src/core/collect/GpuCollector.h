// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/model/Frame.h"

#include <QString>

#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace culprit {

// NVIDIA GPUs through NVML, loaded at runtime with dlopen (no headers or link
// dependency; systems without the driver simply report no GPU).
class Nvml {
public:
    static Nvml& instance();   // loads + initialises on first use (thread-safe)

    bool ok() const { return ok_; }
    int deviceCount() const { return int(devices_.size()); }

    void sample(int index, GpuSample& out);
    // Per-process SM utilisation since the previous call (max per pid).
    void processUtilization(int index, std::unordered_map<int, GpuProc>& out);
    void processMemory(int index, std::vector<GpuProc>& out);   // graphics + compute contexts

    // Our own NVML calls that took 1 ms or more. They hold the driver's lock,
    // so a compositor freeze that coincides with one may be self-inflicted.
    struct BusyInterval {
        int64_t startNs, endNs;
        const char* what;
    };
    std::vector<BusyInterval> busyIntervals(int64_t sinceNs) const;

private:
    Nvml();
    struct BusyScope;
    void noteBusy(int64_t startNs, const char* what);
    struct Api;
    Api* api_ = nullptr;
    bool ok_ = false;
    // Static properties are expensive to query (ms per call) — cache them.
    struct Static {
        bool loaded = false;
        QString name;
        double slowdownC = -1, maxOperatingC = -1, shutdownC = -1;
        int clockSmMaxMHz = -1;
        double powerLimitW = -1;
        int fanPct = -1;
        int64_t slowRefreshNs = 0;   // power limit / fan: refreshed every few seconds
    };
    std::vector<void*> devices_;
    std::vector<Static> static_;
    std::vector<unsigned long long> lastSeenTs_;
    std::mutex mutex_;
    mutable std::mutex busyMutex_;
    std::deque<BusyInterval> busy_;
};

class GpuCollector {
public:
    void sample(Frame& frame);
    // Per-process queries hold the driver's lock for ~2 ms: query less often in the background.
    void setProcRefreshNs(int64_t ns) { procRefreshNs_ = ns; }

private:
    int64_t procRefreshNs_ = 3'000'000'000;
    std::vector<double> idleBaselineW_;   // lowest power seen per GPU: the part not attributable to any app
    // Per-process queries cost ~2.5 ms per GPU (process utilisation alone ~2 ms),
    // so they are refreshed every few seconds and reused in between.
    struct ProcCache {
        int64_t refreshedNs = 0;
        std::vector<GpuProc> procs;
    };
    std::vector<ProcCache> procCache_;
};

} // namespace culprit
