// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/collect/GpuCollector.h"
#include "core/collect/HwmonCollector.h"
#include "core/model/Frame.h"
#include "core/stutter/CompositorProbe.h"
#include "core/stutter/FlightRecorder.h"

#include <deque>

namespace culprit {

// Turns a HitchCapture into an explained Hitch: classification, evidence and
// ranked suspects, by comparing the counters during the stall with the half
// second before it. Compositor freezes are explained from what the frozen
// thread was doing.
class HitchAnalyzer {
public:
    Hitch analyze(const HitchCapture& cap, const Frame* lastFrame, const std::vector<HwmonCollector::BusyInterval>& selfBusy,
                  double tjmaxC);
    Hitch analyzeStall(const CompositorStall& s, const std::vector<Nvml::BusyInterval>& selfGpu);

private:
    // Hitches that repeat on a fixed period ("every 2.02 s").
    struct PeriodDetector {
        double add(int64_t t0);
        std::deque<int64_t> history;   // episode start times
        double lastPeriod = 0;
    };

    PeriodDetector probePeriod_, stallPeriod_;
    uint64_t nextId_ = 1;
};

} // namespace culprit
