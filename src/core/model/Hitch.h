// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/model/Finding.h"

#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

namespace culprit {

enum class HitchClass {
    RunQueue,        // runnable but another task held the CPU
    IrqKernel,       // timer/wakeup delayed: IRQ/softirq work, non-preemptible kernel, firmware
    Reclaim,         // direct memory reclaim / compaction stalled allocations
    Io,              // blocked on I/O / page faults
    Thermal,         // throttling / frequency collapse
    Gpu,             // GPU throttling or driver stall
    CgroupThrottle,  // CPU quota throttling
    Global,          // several CPUs stalled at once (SMI, stop_machine, IPI storm)
    GpuDriver,       // compositor: blocked inside the GPU kernel driver (often on its lock)
    Busy,            // compositor: running its own code the whole time
    GarbageCollection, // compositor: JavaScript garbage collection
    ThreadWait,      // compositor: waiting for another of its threads (futex)
    Unknown
};

QString hitchClassName(HitchClass c);
QString hitchClassDescription(HitchClass c);

// What noticed the hitch.
enum class HitchSource {
    Probe,        // a latency probe woke up late: the system delayed it
    Compositor,   // the Wayland compositor stopped responding: one of its own threads froze
};

// Kernel stack of one thread, read by the root helper while the compositor was frozen.
struct ThreadStack {
    int pid = 0, tid = 0;
    QString comm;      // thread name
    QString process;
    char state = '?';
    QString wchan;
    QStringList frames;
};

// One slice of CPU occupancy during a hitch, reported by the root helper.
struct DeepBlocker {
    QString kind;     // "task", "irq", "softirq"
    int pid = 0;
    int tid = 0;
    QString name;
    double ms = 0;
};

// A latency-probe hitch: a probe thread woke up late by more than the threshold.
// Or a compositor freeze: maxOvershootMs is then how long the frozen thread
// didn't get back to its event loop, runDelayShare the part of that it spent
// waiting for a CPU, and worstCpu/cpus/probesHit are unused.
struct Hitch {
    uint64_t id = 0;
    HitchSource source = HitchSource::Probe;
    int64_t t0Ns = 0;              // first late wakeup (CLOCK_MONOTONIC)
    int64_t t1Ns = 0;              // last late wakeup in this burst
    qint64 wallMs = 0;
    double maxOvershootMs = 0;     // worst wakeup delay
    double runDelayShare = 0;      // fraction of the delay the probe spent runnable on a run queue
    int worstCpu = -1;
    std::vector<int> cpus;         // CPUs whose probes hitched
    int probesHit = 0;
    bool global = false;
    HitchClass cls = HitchClass::Unknown;
    QString summary;               // one-line explanation
    QStringList evidence;
    std::vector<Suspect> suspects; // ranked, most likely first
    double periodSec = 0;          // >0 when part of a periodic pattern
    bool selfActivity = false;     // coincided with Culprit's own slow sensor read

    // Filled in when the root helper answers a window query (probes) or sends
    // the frozen thread's kernel stack (compositor).
    bool deepResolved = false;
    std::vector<DeepBlocker> blockers;

    // Compositor freezes only.
    int pid = 0, tid = 0;
    QString process;               // "gnome-shell"
    QString thread;                // "main thread", "KMS thread"
    double onCpuMs = 0, runqMs = 0, blockedMs = 0;   // where the freeze went
    QStringList blockedIn;         // where it slept, with the share of samples
    std::vector<ThreadStack> stacks;
};

} // namespace culprit
