// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/collect/HwmonCollector.h"
#include "core/model/Frame.h"
#include "core/stutter/FlightRecorder.h"

#include <deque>

namespace culprit {

// Turns a HitchCapture into an explained Hitch: classification, evidence and
// ranked suspects, by comparing the counters during the stall with the half
// second before it.
class HitchAnalyzer {
public:
    Hitch analyze(const HitchCapture& cap, const Frame* lastFrame, const std::vector<HwmonCollector::BusyInterval>& selfBusy,
                  double tjmaxC);

private:
    double detectPeriod(int64_t t0);

    std::deque<int64_t> history_;   // episode start times
    double lastPeriod_ = 0;
    uint64_t nextId_ = 1;
};

} // namespace culprit
