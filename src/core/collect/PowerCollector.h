// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "common/fs/File.h"
#include "core/model/Frame.h"

#include <algorithm>
#include <string>
#include <vector>

namespace culprit {

// CPU energy counters from the powercap (RAPL) interface, as watts.
// On AMD Zen the "intel-rapl" zones are package-N and core; uncore is derived.
class PowerCollector {
public:
    PowerCollector();
    void sample(PowerSample& out, int64_t nowNs);
    bool readable() const { return !zones_.empty(); }

    // The idle floor is remembered across runs so a session that starts under
    // load still attributes power sensibly.
    void setKnownFloor(double w) { if (w > 0) floorW_ = std::min(floorW_, w); }
    double floor() const { return floorW_ < 1e8 ? floorW_ : -1; }

private:
    enum class Kind { Package, Core, Uncore, Dram, Other };
    struct Zone {
        CachedFile energy;
        Kind kind = Kind::Other;
        uint64_t maxRangeUj = 0;
        uint64_t lastUj = 0;
        bool have = false;
    };
    std::vector<Zone> zones_;
    int64_t lastNs_ = 0;
    double floorW_ = 1e9;
    bool coreZoneTrustworthy_ = true;
};

} // namespace culprit
