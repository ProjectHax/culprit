// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include <cstdint>
#include <ctime>

namespace culprit {

inline int64_t clockNs(clockid_t id)
{
    timespec ts{};
    clock_gettime(id, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// All Culprit timestamps (GUI, probes, helper perf events) use CLOCK_MONOTONIC.
inline int64_t monoNs() { return clockNs(CLOCK_MONOTONIC); }
inline int64_t bootNs() { return clockNs(CLOCK_BOOTTIME); }

inline timespec nsToTimespec(int64_t ns)
{
    return timespec{time_t(ns / 1'000'000'000), long(ns % 1'000'000'000)};
}

} // namespace culprit
