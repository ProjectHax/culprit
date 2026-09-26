// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/Settings.h"
#include "core/model/Frame.h"

#include <QHash>

#include <memory>

namespace culprit {

struct RuleState;

// Runs every diagnosis rule on each frame and tracks findings over time:
// a finding is shown after it has been raised on a few consecutive
// evaluations and cleared after a few clean ones (hysteresis), so the list
// doesn't flicker. Cleared findings stay listed (inactive) for a while.
class RuleEngine {
public:
    explicit RuleEngine(const Settings& settings);
    ~RuleEngine();

    void setSettings(const Settings& s) { settings_ = s; }
    void evaluate(Frame& frame);
    void noteHitch(const Hitch& h);

    // Transitions from the last evaluate() call, for the recorder.
    const std::vector<Finding>& raised() const { return raised_; }
    const std::vector<Finding>& cleared() const { return cleared_; }

private:
    struct Tracked {
        Finding finding;
        int hits = 0;
        int misses = 0;
        bool shown = false;
        int64_t clearedNs = 0;
    };

    Settings settings_;
    std::unique_ptr<RuleState> state_;
    QHash<QString, Tracked> tracked_;
    std::vector<Finding> raised_, cleared_;
};

} // namespace culprit
