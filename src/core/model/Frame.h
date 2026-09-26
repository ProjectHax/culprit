// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

// One sampling interval's worth of everything Culprit knows, published by the
// Engine as an immutable shared_ptr<const Frame>.

#include "core/model/Finding.h"
#include "core/model/Hitch.h"

#include <QMetaType>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace culprit {

// Identifies a process instance: a pid plus its start time, so a recycled pid
// is never confused with the process that previously had it.
struct ProcKey {
    int pid = 0;
    uint64_t starttime = 0;
    bool operator==(const ProcKey&) const = default;
};

struct ProcKeyHash {
    size_t operator()(const ProcKey& k) const noexcept
    {
        return std::hash<uint64_t>()((uint64_t(uint32_t(k.pid)) << 40) ^ k.starttime);
    }
};

// ---------------------------------------------------------------- system
struct CpuLoad {   // percent of one CPU (0..100)
    float user = 0, system = 0, irq = 0, softirq = 0, iowait = 0, steal = 0, busy = 0;
};

struct CpuCoreSample {
    int cpu = 0;
    bool online = false;
    CpuLoad load;
    float freqMHz = 0;
    float runDelayPct = 0;   // time tasks waited for this CPU / wall time, in % (100 = one task always waiting)
    double irqPs = 0;        // hardware interrupts per second on this CPU
    double softirqPs = 0;
};

struct MemSample {
    uint64_t totalKb = 0, availableKb = 0, usedKb = 0, cachedKb = 0, buffersKb = 0;
    uint64_t swapTotalKb = 0, swapUsedKb = 0, dirtyKb = 0, writebackKb = 0, shmemKb = 0;
};

struct VmRates {   // per second
    double pgfault = 0, pgmajfault = 0, pswpin = 0, pswpout = 0;
    double pgscanDirect = 0, pgstealDirect = 0, pgscanKswapd = 0;
    double allocstall = 0, compactStall = 0, oomKill = 0, workingsetRefault = 0;
    double pgpginKBs = 0, pgpgoutKBs = 0;
};

struct DiskSample {
    QString name;        // kernel name (nvme0n1, dm-0)
    QString label;       // friendlier name if known (dm name)
    double readsPs = 0, writesPs = 0, readBps = 0, writeBps = 0;
    double utilPct = 0, awaitMs = 0, readAwaitMs = 0, writeAwaitMs = 0;
    uint64_t inFlight = 0;
};

struct NetSample {
    QString name;
    double rxBps = 0, txBps = 0, rxPps = 0, txPps = 0;
    double errsPs = 0, dropsPs = 0;
};

struct IrqRate {
    QString label, name;
    double perSec = 0;
    int topCpu = -1;
    double topCpuPerSec = 0;
};

struct SoftirqRate {
    int vec = 0;
    QString name;
    double perSec = 0;
    int topCpu = -1;
    double topCpuPerSec = 0;
};

struct PsiLine {
    double avg10 = 0, avg60 = 0;
};

struct PsiSample {
    bool available = false;
    PsiLine cpuSome, memSome, memFull, ioSome, ioFull;
};

struct CpuPolicy {
    QString driver, governor, epp;
    int boost = -1;          // -1 unknown, 0 off, 1 on
    double hwMaxMHz = 0, hwMinMHz = 0, scalingMaxMHz = 0;
};

struct SystemSample {
    int ncpu = 0;            // highest CPU index + 1
    int onlineCpus = 0;
    double intervalSec = 0;
    double uptimeSec = 0;
    CpuLoad total;
    std::vector<CpuCoreSample> cpus;
    double load1 = 0, load5 = 0, load15 = 0;
    uint32_t procsRunning = 0, procsBlocked = 0, threadsTotal = 0;
    double avgRunnable = -1, avgBlocked = -1;   // averaged at 50 Hz by the flight recorder, -1 if unknown
    double ctxtPs = 0, intrPs = 0, forksPs = 0;
    double runDelayTotalPct = 0;                // sum over CPUs: avg number of waiting tasks x100
    MemSample mem;
    VmRates vm;
    std::vector<DiskSample> disks;
    std::vector<NetSample> nets;
    std::vector<IrqRate> irqs;                  // busiest first
    std::vector<SoftirqRate> softirqs;
    PsiSample psi;
    CpuPolicy policy;
};

// ---------------------------------------------------------------- sensors / power / gpu
enum class SensorKind { Temp, Fan, Voltage, Power, Current, Other };

struct SensorReading {
    QString chip;            // hwmon name, e.g. "k10temp"
    QString label;           // e.g. "Tctl"
    QString key;             // stable id, e.g. "k10temp/temp1"
    SensorKind kind = SensorKind::Other;
    double value = 0;        // °C, RPM, V, W, A
    double crit = 0;         // 0 = unknown
    double max = 0;
    bool slowChip = false;   // read on the slow tier (expensive Super-I/O chip)
};

struct PowerSample {
    bool available = false;
    double packageW = -1, coreW = -1, uncoreW = -1, dramW = -1;
    double idleFloorW = -1;       // lowest package power seen this session
    double attributableW = -1;    // the part processes can be blamed for (core power, or package above idle floor)
};

struct GpuProc {
    int pid = 0;
    unsigned smUtil = 0, memUtil = 0, encUtil = 0, decUtil = 0;
    uint64_t usedMemBytes = 0;
};

struct GpuSample {
    int index = 0;
    QString name;
    double tempC = -1, slowdownC = -1, maxOperatingC = -1, shutdownC = -1;
    double powerW = -1, powerLimitW = -1;
    int utilGpu = -1, utilMem = -1, fanPct = -1;
    uint64_t memUsed = 0, memTotal = 0;
    int clockSmMHz = -1, clockMemMHz = -1, clockSmMaxMHz = -1;
    uint64_t eventReasons = 0;
    bool reasonsValid = false;
    std::vector<GpuProc> procs;
};

// NVML clock event ("throttle") reason bits.
namespace GpuReason {
inline constexpr uint64_t Idle = 0x1, AppClocks = 0x2, SwPowerCap = 0x4, HwSlowdown = 0x8,
                          SyncBoost = 0x10, SwThermal = 0x20, HwThermal = 0x40,
                          HwPowerBrake = 0x80, DisplayClocks = 0x100;
}
QStringList decodeGpuEventReasons(uint64_t reasons);

inline constexpr double kNoTemp = -1000;   // "no reading" sentinel for temperatures

struct ThermalSummary {
    double cpuTempC = kNoTemp;
    QString cpuTempLabel;    // "k10temp Tctl"
    double tjmaxC = 90;
    double gpuTempC = kNoTemp;
    bool hasCpuTemp() const { return cpuTempC > kNoTemp + 1; }
};

// ---------------------------------------------------------------- processes
struct ThreadSample {
    int tid = 0;
    QString comm;
    char state = '?';
    double cpuPct = 0;
    double runDelayMsPs = -1;   // ms spent waiting on a run queue per second
    double nivcswPs = -1;       // involuntary context switches per second
    int lastCpu = -1;
    int nice = 0;
    int policy = 0;
    int rtPrio = 0;
};

struct ProcSample {
    ProcKey key;
    int ppid = 0;
    QString comm;
    QString cmdline;
    QString cgroup;
    QString unit;             // systemd unit/scope from the cgroup path
    uint32_t uid = 0;
    QString user;
    char state = '?';
    bool kernelThread = false;
    double cpuPct = 0;        // % of one CPU (top-style; can exceed 100)
    double runDelayMsPs = -1; // summed over threads, hot set only (-1 = not measured)
    double nivcswPs = -1;     // involuntary ctx switches/s (hot set only)
    double nvcswPs = -1;
    double majfltPs = 0, minfltPs = 0;
    uint64_t rssBytes = 0;
    int threads = 0;
    int nice = 0;
    int policy = 0;           // SCHED_OTHER=0, FIFO=1, RR=2, BATCH=3, IDLE=5, DEADLINE=6
    int rtPrio = 0;
    int lastCpu = -1;
    double ioReadBps = -1, ioWriteBps = -1;   // -1 = not readable
    double blkioMsPs = -1;                    // delayacct block-I/O wait ms/s (needs task_delayacct)
    double gpuPct = -1;
    uint64_t gpuMemBytes = 0;
    double estCpuW = -1, estGpuW = -1;        // estimated power share
    int dThreads = 0;                         // threads in D state (when scanned)
    bool hot = false;                         // got a per-thread scan this interval
    QString wchan;                            // kernel wait channel (leader), when visible
    std::vector<ThreadSample> threadDetails;  // only for processes the UI asked about
};

struct DStateThread {
    int pid = 0, tid = 0;
    QString comm;          // thread name
    QString procComm;      // owning process name
    QString wchan;
    QStringList stack;     // kernel stack (root helper only)
    double ageSec = 0;     // how long it has been seen in D
};

struct CgroupSample {
    QString path, unit;
    double throttledPct = 0;      // % of CFS periods throttled over the interval
    double throttledMsPs = 0;
    double cpuQuotaCores = -1;    // cpu.max quota / period, -1 = unlimited
    uint64_t memHighEvents = 0, memMaxEvents = 0, oomKills = 0;   // deltas this interval
    double memHighBytes = -1;
};

// ---------------------------------------------------------------- helper / probes status
struct HelperStatus {
    enum class State { Off, Starting, Running, Failed } state = State::Off;
    QString message;
    uint64_t lostEvents = 0;
    double eventsPerSec = 0;
    double cpuPct = 0;          // the helper's own CPU usage
    QStringList caps;
};

// Deep-trace (root helper) results.
struct DeepEvent {
    QString kind;               // rqlat, irq, softirq, reclaim, compact, blk
    int64_t tNs = 0;
    qint64 wallMs = 0;
    int cpu = -1;
    int pid = 0, tid = 0;
    QString name;               // task comm / IRQ name / softirq vector / device
    double ms = 0;
    QString detail;             // e.g. blockers summary
};

struct DeepWaiter {
    int pid = 0, tid = 0;
    QString comm;
    int count = 0;
    double sumMs = 0, maxMs = 0;
};

struct DeepSummary {
    std::vector<DeepWaiter> topWaiters;   // last second
    std::vector<double> rqHist, irqHist, softirqHist;   // log2(µs) buckets, last second
    std::vector<DeepEvent> events;        // new since the previous frame
};

struct StutterSummary {
    int mode = 0;               // ProbeMode
    int probes = 0;
    double thresholdMs = 0;
    double worstMs = -1;        // worst probe wakeup delay during this interval
    uint64_t hitchesTotal = 0;
    uint64_t suppressed = 0;    // hitches not analysed (rate limit)
    QString error;
};

struct SelfStats {
    double cpuPct = 0;        // Culprit's own CPU usage (all threads)
    uint64_t rssBytes = 0;
    double collectMs = 0;     // time spent building this frame
};

struct Frame {
    uint64_t seq = 0;
    int64_t tNs = 0;           // CLOCK_MONOTONIC
    qint64 wallMs = 0;
    SystemSample sys;
    std::vector<SensorReading> sensors;
    PowerSample power;
    std::vector<GpuSample> gpus;
    ThermalSummary thermal;
    std::vector<ProcSample> procs;
    std::vector<DStateThread> dstate;
    std::vector<CgroupSample> cgroups;
    std::vector<Finding> findings;   // active first, then recently cleared
    HelperStatus helper;
    DeepSummary deep;
    StutterSummary stutter;
    SelfStats self;

    const ProcSample* findProc(int pid) const;
};

using FramePtr = std::shared_ptr<const Frame>;

} // namespace culprit

Q_DECLARE_METATYPE(culprit::FramePtr)
Q_DECLARE_METATYPE(culprit::Hitch)
