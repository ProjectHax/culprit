// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/model/Frame.h"

#include <QCoreApplication>

namespace culprit {

QString severityName(Severity s)
{
    switch (s) {
    case Severity::Info: return QStringLiteral("Info");
    case Severity::Warning: return QStringLiteral("Warning");
    case Severity::Critical: return QStringLiteral("Critical");
    }
    return {};
}

QString hitchClassName(HitchClass c)
{
    switch (c) {
    case HitchClass::RunQueue: return QStringLiteral("CPU contention");
    case HitchClass::IrqKernel: return QStringLiteral("IRQ / kernel");
    case HitchClass::Reclaim: return QStringLiteral("Memory reclaim");
    case HitchClass::Io: return QStringLiteral("I/O / page faults");
    case HitchClass::Thermal: return QStringLiteral("Thermal / frequency");
    case HitchClass::Gpu: return QStringLiteral("GPU");
    case HitchClass::CgroupThrottle: return QStringLiteral("Cgroup CPU quota");
    case HitchClass::Global: return QStringLiteral("System-wide stall");
    case HitchClass::Unknown: return QStringLiteral("Unknown");
    }
    return {};
}

QString hitchClassDescription(HitchClass c)
{
    switch (c) {
    case HitchClass::RunQueue:
        return QStringLiteral("The thread was runnable but had to wait while other tasks used the CPU.");
    case HitchClass::IrqKernel:
        return QStringLiteral("The wakeup itself was delayed: interrupt/softirq processing, a long "
                              "non-preemptible kernel section, or firmware (SMI) held the CPU.");
    case HitchClass::Reclaim:
        return QStringLiteral("Memory allocations stalled in direct reclaim or compaction.");
    case HitchClass::Io:
        return QStringLiteral("Tasks blocked on storage I/O or major page faults.");
    case HitchClass::Thermal:
        return QStringLiteral("The CPU was at its thermal limit or its clock dropped sharply.");
    case HitchClass::Gpu:
        return QStringLiteral("The GPU reported thermal/power throttling around the stall.");
    case HitchClass::CgroupThrottle:
        return QStringLiteral("A cgroup CPU quota throttled tasks for the rest of its period.");
    case HitchClass::Global:
        return QStringLiteral("Several CPUs stalled at the same moment: typical of SMIs/firmware, "
                              "stop_machine, TLB shootdown storms or a driver disabling interrupts.");
    case HitchClass::Unknown:
        return QStringLiteral("No single cause stood out in the sampled counters.");
    }
    return {};
}

QStringList decodeGpuEventReasons(uint64_t r)
{
    QStringList out;
    if (r & GpuReason::Idle) out << QStringLiteral("Idle");
    if (r & GpuReason::AppClocks) out << QStringLiteral("App clock setting");
    if (r & GpuReason::SwPowerCap) out << QStringLiteral("Power cap");
    if (r & GpuReason::HwSlowdown) out << QStringLiteral("HW slowdown");
    if (r & GpuReason::SyncBoost) out << QStringLiteral("Sync boost");
    if (r & GpuReason::SwThermal) out << QStringLiteral("SW thermal slowdown");
    if (r & GpuReason::HwThermal) out << QStringLiteral("HW thermal slowdown");
    if (r & GpuReason::HwPowerBrake) out << QStringLiteral("HW power brake");
    if (r & GpuReason::DisplayClocks) out << QStringLiteral("Display clock setting");
    return out;
}

const ProcSample* Frame::findProc(int pid) const
{
    for (const auto& p : procs) {
        if (p.key.pid == pid)
            return &p;
    }
    return nullptr;
}

} // namespace culprit
