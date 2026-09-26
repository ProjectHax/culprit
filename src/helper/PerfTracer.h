// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "common/trace/TraceFormat.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace culprit {

// A tracepoint sample copied out of a perf ring buffer.
struct TraceEvent {
    uint64_t timeNs = 0;   // CLOCK_MONOTONIC
    int tp = -1;           // index into PerfTracer::tracepoints()
    int32_t cpu = -1;
    int32_t pid = 0;       // tgid of the task current when the event fired
    int32_t tid = 0;
    uint16_t rawLen = 0;
    uint8_t raw[160];
};

// System-wide tracepoint recording with perf_event_open: one fd per
// (tracepoint, CPU), one mmap'ed ring per CPU (other events redirected into it
// with PERF_EVENT_IOC_SET_OUTPUT), timestamps on CLOCK_MONOTONIC so they line
// up with the GUI's probe timestamps.
class PerfTracer {
public:
    struct Tracepoint {
        std::string sys, name;
        TraceFormat fmt;
    };

    ~PerfTracer();

    // Opens every wanted tracepoint that exists; returns false only if none could be opened.
    bool open(const std::string& tracefs, const std::vector<std::pair<std::string, std::string>>& wanted, int pagesPerCpu,
              std::string& error);
    void enable();
    void disable();
    void close();
    bool isOpen() const { return !rings_.empty(); }

    const std::vector<Tracepoint>& tracepoints() const { return tps_; }
    int indexOf(const std::string& sys, const std::string& name) const;

    // Copies out all complete records. Samples are appended to `out` (unsorted
    // across CPUs); lost-record counts are added per CPU.
    void drain(std::vector<TraceEvent>& out, std::vector<uint64_t>& lostPerCpu);
    int maxCpu() const { return maxCpu_; }

private:
    struct Ring {
        int cpu = -1;
        int leader = -1;
        std::vector<int> fds;
        uint8_t* base = nullptr;
        size_t mapLen = 0;
        uint8_t* data = nullptr;
        uint64_t dataSize = 0;
    };

    std::vector<Tracepoint> tps_;
    std::vector<Ring> rings_;
    std::unordered_map<uint64_t, int> idToTp_;
    std::vector<uint8_t> scratch_;
    int maxCpu_ = 0;
};

std::vector<int> onlineCpus();

} // namespace culprit
