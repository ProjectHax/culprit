// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "common/fs/File.h"
#include "core/model/Frame.h"

#include <string>
#include <unordered_map>

namespace culprit {

// cgroup v2 CPU-quota throttling and memory.high/max events for the cgroups
// that busy processes live in (and their ancestors — a quota can sit on a
// parent slice).
class CgroupCollector {
public:
    void sample(Frame& frame, int64_t nowNs);

private:
    struct State {
        CachedFile cpuStat, memEvents, cpuMax, memHigh;
        uint64_t periods = 0, throttled = 0, throttledUs = 0;
        uint64_t high = 0, max = 0, oom = 0;
        bool have = false;
        int64_t seenNs = 0;
    };
    std::unordered_map<std::string, State> cgroups_;
    std::string buf_;
};

} // namespace culprit
