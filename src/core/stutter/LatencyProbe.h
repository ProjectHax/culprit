// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "common/util/SpscRing.h"
#include "core/Settings.h"

#include <QString>

#include <atomic>
#include <memory>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace culprit {

// One wakeup of a probe thread.
struct ProbeSample {
    int64_t expectedNs = 0;      // when the timer should have woken us (CLOCK_MONOTONIC)
    int64_t actualNs = 0;        // when we actually ran
    int64_t runDelayNs = 0;      // how much of that we spent runnable on a run queue
    int32_t cpu = -1;            // CPU we woke up on
    int16_t probe = 0;
    int16_t flags = 0;           // kSuspended: system suspend in between, ignore

    static constexpr int16_t kSuspended = 1;
    int64_t overshootNs() const { return actualNs - expectedNs; }
};

// A set of latency probe threads. Each sleeps until an absolute deadline with
// 1 ns timer slack, then records how late it woke up and — from its own
// /proc/thread-self/schedstat — how much of that lateness was spent waiting
// runnable on a run queue (CPU contention) versus before being woken at all
// (timer/IRQ/kernel/firmware delay).
class ProbeSet {
public:
    struct Config {
        ProbeMode mode = ProbeMode::Floating;
        int count = 4;           // floating mode
        int periodUs = 1000;
    };

    ProbeSet() = default;
    ~ProbeSet();

    bool start(const Config& cfg, int ncpu, QString* error);
    void stop();
    bool running() const { return !probes_.empty(); }
    int count() const { return int(probes_.size()); }
    std::vector<pid_t> tids() const;
    uint64_t dropped() const;

    template <typename F>
    void drain(F&& f)
    {
        ProbeSample s;
        for (auto& p : probes_)
            while (p->ring.pop(s))
                f(s);
    }

private:
    struct Probe {
        std::thread thread;
        SpscRing<ProbeSample, 8192> ring;
        std::atomic<pid_t> tid{0};
        std::atomic<uint64_t> dropped{0};
        int cpu = -1;            // pinned CPU or -1
        int id = 0;
    };
    void run(Probe* p, int64_t periodNs);

    std::vector<std::unique_ptr<Probe>> probes_;
    std::atomic<bool> stop_{false};
};

// Asks rtkit (org.freedesktop.RealtimeKit1) to make a thread SCHED_FIFO.
bool makeThreadRealtime(pid_t tid, int priority, QString* error);

} // namespace culprit
