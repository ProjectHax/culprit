// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/analysis/Attribution.h"

#include <algorithm>

namespace culprit {

void attributeCpuPower(Frame& f)
{
    // Core power is what processes cause; uncore (fabric, memory controller,
    // I/O die) is shared and mostly static, so it isn't handed out. Without a
    // usable core counter, the package power above the idle floor is used.
    const double watts = f.power.attributableW;
    if (watts < 0 || f.procs.empty())
        return;

    double avgFreq = 0;
    int n = 0;
    for (const CpuCoreSample& c : f.sys.cpus)
        if (c.online && c.freqMHz > 0) {
            avgFreq += c.freqMHz;
            ++n;
        }
    avgFreq = n ? avgFreq / n : 1000.0;

    auto weight = [&](const ProcSample& p) {
        if (p.cpuPct <= 0)
            return 0.0;
        double freq = avgFreq;
        if (p.lastCpu >= 0 && p.lastCpu < int(f.sys.cpus.size()) && f.sys.cpus[size_t(p.lastCpu)].freqMHz > 0)
            freq = f.sys.cpus[size_t(p.lastCpu)].freqMHz;
        return p.cpuPct * freq;
    };

    double total = 0;
    for (const ProcSample& p : f.procs)
        total += weight(p);
    if (total <= 0)
        return;
    for (ProcSample& p : f.procs) {
        const double w = weight(p);
        p.estCpuW = w > 0 ? watts * w / total : 0.0;
    }
}

} // namespace culprit
