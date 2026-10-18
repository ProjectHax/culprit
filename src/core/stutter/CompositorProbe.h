// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "common/fs/File.h"
#include "core/model/Frame.h"
#include "core/stutter/FlightRecorder.h"

#include <QString>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace culprit {

// What one of the compositor's other threads did during a freeze.
struct ThreadActivity {
    QString name;
    double runMs = 0, waitMs = 0;   // on a CPU / runnable, waiting for one
    QString blockedIn;              // where it slept most, if it slept
    int blockedSamples = 0, samples = 0;
};

// A thread (of any GPU client) seen inside the GPU driver while the compositor was frozen.
struct DriverThread {
    int pid = 0, tid = 0;
    QString process, thread;
    QString call;                   // "ioctl /dev/nvidiactl NV RM_CONTROL"; empty if not seen in a syscall
    QString wchan;                  // where it slept; empty while it ran
    bool waitingForLock = false;    // asleep on one of the NVIDIA driver's locks
    double kernelMs = -1;           // CPU time it spent in the kernel during the rest of the freeze
};

// One freeze, as measured by the CompositorProbe.
struct CompositorStall {
    uint64_t seq = 0;               // same as in the earlier "still frozen" notice
    int pid = 0, tid = 0;
    QString process, thread;
    bool mainThread = false;        // found by an unanswered Wayland ping
    int64_t t0Ns = 0, t1Ns = 0;
    double runMs = 0, waitMs = 0;
    double helperMs = 0;            // CPU time of its JavaScript helper (GC) threads
    int samples = 0;
    std::vector<std::pair<QString, int>> blockedIn;   // where it slept: sample counts, most first
    std::vector<ThreadActivity> others;
    std::vector<DriverThread> driver;
    double selfLateMs = 0;          // the probe's own worst wakeup delay meanwhile
    uint64_t compactStalls = 0, allocStalls = 0, majorFaults = 0;
};

// Watches the Wayland compositor for freezes, unprivileged. Its main thread
// answers wl_display.sync on a connection of our own (a "ping"), so a late
// answer means its event loop was stuck. The main thread and the threads that
// move the cursor and flip frames (KMS, input) are sampled at 25 Hz, and at
// 100 Hz while one may be stuck: time on a CPU and on a run queue from
// schedstat and, while they sleep anywhere but their event loop, the kernel
// wait site and the syscall (with the device and request of an ioctl).
class CompositorProbe {
public:
    using StallFn = std::function<void(std::shared_ptr<CompositorStall>)>;
    // A freeze passed the threshold and is still going: kernel stacks of these
    // threads (Deep trace) would show where they are stuck.
    using OngoingFn = std::function<void(uint64_t seq, std::vector<int> tids)>;

    CompositorProbe(std::shared_ptr<LatencyFeed> feed, StallFn onStall, OngoingFn onOngoing);
    ~CompositorProbe();

    void start(double thresholdMs);
    void stop();
    // Processes with GPU contexts (from NVML): who else is in the driver during a driver freeze.
    void setGpuClients(std::vector<int> pids);
    void summary(CompositorSummary& out);   // also resets the per-interval worst response

private:
    struct Thread;
    struct Window;
    struct DriverScan;

    void loop();
    bool connect(QString& why);
    void disconnect();
    void watch();
    bool receive(int64_t now);
    bool sendPing(int64_t now);
    bool tick(int64_t now);
    bool refreshThreads(int64_t now);
    bool sample(Thread& t, int64_t now);
    bool track(Thread& t, int64_t now);
    bool stuck(const Thread& t, int64_t a, int64_t b) const;
    bool duringMainFreeze(int64_t a, int64_t b) const;
    int64_t stallStart(const Thread& main, int64_t sentNs, int64_t prevDoneNs) const;
    Window window(const Thread& t, int64_t a, int64_t b) const;
    uint64_t ongoing(const Thread& t);
    void finish(const Thread& t, int64_t t0, int64_t t1, uint64_t seq);
    DriverScan scanDriver(int skipTid);
    void kernelTimes(const DriverScan& scan, int64_t endNs, std::vector<DriverThread>& out);
    void sampleVm(int64_t now);
    void forgetSpans(int64_t now);
    std::string describe(Thread& t, std::string_view wchan);
    std::string fdTarget(int fd);
    uint16_t label(const std::string& s);
    Thread* mainThread() const;
    void setStatus(bool watching, const QString& message);

    std::shared_ptr<LatencyFeed> feed_;
    StallFn onStall_;
    OngoingFn onOngoing_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    int64_t thresholdNs_ = 100'000'000;

    std::mutex mutex_;   // guards the three members below
    std::vector<int> gpuClients_;
    CompositorSummary status_;
    std::deque<std::pair<int64_t, int64_t>> rtts_;   // (answered at, round trip) over the last second

    // probe thread only
    int sock_ = -1;
    int pid_ = 0;
    std::string comm_;
    std::string rx_;   // partial Wayland messages
    std::string buf_, buf2_;
    std::vector<std::unique_ptr<Thread>> threads_;
    std::unordered_set<int> ignored_;   // threads that aren't worth sampling
    std::vector<std::string> labels_;
    std::unordered_map<std::string, uint16_t> labelIds_;
    std::unordered_map<int, std::string> fdCache_;
    int64_t fdCacheNs_ = 0;
    int64_t pingSentNs_ = 0, lastPingNs_ = 0;
    int64_t lastDoneNs_ = 0;       // when the last ping was answered: the event loop ran then
    int64_t pingPrevDoneNs_ = 0;   // lastDoneNs_ when the outstanding ping was sent
    int64_t lastTasksNs_ = 0, lastVmNs_ = 0, lastHelperNs_ = 0, lastFeedNs_ = 0;
    bool dense_ = false;         // sample at 100 Hz: something may be stuck
    bool callbackFree_ = true;
    bool discardPing_ = false;   // sent before a suspend: its round trip means nothing
    struct Pong {
        int64_t sentNs, doneNs, prevDoneNs;
    };
    std::vector<Pong> pongs_;   // answered pings, handled after the next samples
    uint64_t mainSeq_ = 0;       // "still frozen" notice sent for the outstanding ping
    int64_t lastMainT0_ = 0, lastMainT1_ = 0;
    uint64_t seq_ = 0;
    int64_t bucketWorstNs_ = 0;  // live graph: worst response since the last point
    int64_t suspendOffsetNs_ = 0;
    std::unordered_map<uint64_t, std::unique_ptr<DriverScan>> driverScans_;
    std::deque<std::pair<int64_t, int64_t>> late_;   // (tick, how late we woke for it)
    struct VmPoint {
        int64_t tNs;
        uint64_t compact, alloc, majflt;
    };
    std::deque<VmPoint> vm_;
    CachedFile vmFile_;
};

} // namespace culprit
