// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/model/Frame.h"

#include <QString>

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
    // Cheap (~6 µs) query for the flight recorder.
    bool eventReasons(int index, uint64_t& reasons);
    // Per-process SM utilisation since the previous call (max per pid).
    void processUtilization(int index, std::unordered_map<int, GpuProc>& out);

private:
    Nvml();
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
};

class GpuCollector {
public:
    void sample(Frame& frame);

private:
    std::vector<double> idleBaselineW_;   // lowest power seen per GPU: the part not attributable to any app
};

} // namespace culprit
