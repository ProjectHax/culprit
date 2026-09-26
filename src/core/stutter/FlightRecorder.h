// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "common/fs/File.h"
#include "common/parse/SystemParsers.h"
#include "core/Settings.h"
#include "core/stutter/LatencyProbe.h"

#include <QString>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace culprit {

// One 20 ms tick of cheap system counters. `valid` says which groups were
// read on this tick (they run at different rates).
struct FlightSample {
    enum : uint32_t { kSched = 1, kStat = 2, kSoftirq = 4, kVm = 8, kTemp = 16, kIrq = 32, kFreq = 64, kGpu = 128 };
    int64_t tNs = 0;
    uint32_t valid = 0;
    uint16_t procsRunning = 0, procsBlocked = 0;
    uint64_t forks = 0;   // /proc/stat "processes" (cumulative)
    float tctl = 0;
    uint64_t gpuReasons = 0;
    VmStat vm;
    std::vector<uint64_t> runNs, waitNs;                                  // per CPU, /proc/schedstat
    std::vector<uint64_t> busyTicks, irqTicks, softirqTicks, totalTicks;  // per CPU, /proc/stat
    std::vector<uint64_t> softirq;                                        // [vec * ncpu + cpu]
    std::vector<uint32_t> freqKHz;                                        // per CPU
};

struct IrqSnapshot {
    int64_t tNs = 0;
    std::shared_ptr<const std::vector<std::pair<QString, QString>>> lines;   // (label, name)
    int ncpu = 0;
    std::vector<uint64_t> counts;   // [line * ncpu + cpu]
};

struct HotProc {
    int pid = 0;
    uint64_t ticks = 0;   // utime + stime
    int cpu = -1;
    int policy = 0;
};

struct HotSnapshot {
    int64_t tNs = 0;
    std::vector<HotProc> procs;
};

// Everything around one hitch, handed to the HitchAnalyzer.
struct HitchCapture {
    std::vector<ProbeSample> hits;        // wakeups over the threshold
    std::vector<FlightSample> window;     // ~ -600 ms .. +200 ms
    std::vector<IrqSnapshot> irqs;
    std::vector<HotSnapshot> procs;
    int ncpu = 0;
    int probeCount = 0;
    double thresholdMs = 0;
    ProbeMode mode = ProbeMode::Floating;
};

// Live latency series for the GUI (thread-safe).
class LatencyFeed {
public:
    struct Point {
        int64_t tNs = 0;
        float maxUs = 0;        // worst probe wakeup delay in this 20 ms tick
        float rqShare = 0;      // share of that delay spent on a run queue
    };
    void push(const Point& p);
    void copySince(int64_t sinceNs, std::vector<Point>& out) const;
    // Worst delay since the last call (for the per-second Frame summary).
    float takeWorstUs();

    std::atomic<int> probes{0};
    std::atomic<int> mode{0};
    std::atomic<double> thresholdMs{0};
    std::atomic<uint64_t> hitches{0};
    std::atomic<uint64_t> suppressed{0};

private:
    mutable std::mutex m_;
    std::deque<Point> pts_;
    float worstUs_ = 0;
};

class FlightRecorder {
public:
    using CaptureFn = std::function<void(std::shared_ptr<HitchCapture>)>;

    FlightRecorder(std::shared_ptr<LatencyFeed> feed, CaptureFn onCapture);
    ~FlightRecorder();

    bool start(const Settings& settings, const std::string& cpuTempPath, int ncpu, QString* error);
    void stop();
    bool applyProbeMode(const Settings& settings, QString* error);   // restart probes
    void setHotPids(std::vector<int> pids);
    void takeAverages(double& runnable, double& blocked);
    std::vector<pid_t> probeTids() const;

private:
    struct Pending {
        std::vector<ProbeSample> hits;
        int64_t firstNs = 0, lastNs = 0;
    };

    void loop();
    void tick(int64_t now, uint64_t n);
    void readSched(FlightSample& s);
    void readStat(FlightSample& s);
    void readSoftirq(FlightSample& s);
    void readIrqs(int64_t now);
    void readHot(int64_t now);
    void processProbes(int64_t now);
    void finalize(Pending& p, int64_t now);
    void initSample(FlightSample& s) const;

    std::shared_ptr<LatencyFeed> feed_;
    CaptureFn onCapture_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    int ncpu_ = 0;

    mutable std::mutex probeMutex_;
    ProbeSet probes_;
    ProbeMode mode_ = ProbeMode::Floating;
    int64_t thresholdNs_ = 2'000'000;

    // rings (recorder thread only, except when copied into a capture)
    std::vector<FlightSample> ring_;
    size_t head_ = 0, filled_ = 0;
    std::deque<IrqSnapshot> irqRing_;
    std::deque<HotSnapshot> hotRing_;
    std::vector<Pending> pending_;
    std::deque<int64_t> recentCaptures_;

    // readers
    std::string buf_;
    CachedFile statFile_, schedFile_, softirqFile_, vmFile_, irqFile_, tempFile_;
    std::vector<CachedFile> freqFiles_;
    ProcStat stat_;
    std::vector<CpuSchedstat> sched_;
    Softirqs softirqs_;
    Interrupts interrupts_;
    std::shared_ptr<std::vector<std::pair<QString, QString>>> irqLines_;
    std::vector<std::string> irqLabels_;

    std::mutex hotMutex_;
    std::vector<int> hotPids_;

    std::mutex avgMutex_;
    double sumRunnable_ = 0, sumBlocked_ = 0;
    int avgCount_ = 0;
};

} // namespace culprit
