// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

// Heat diagnosis: what is heating the machine, and is anything throttling.

#include "core/analysis/rules/Rules.h"

#include "core/Format.h"

#include <algorithm>

namespace culprit {

namespace {

double busyCoreAvgFreq(const SystemSample& s)
{
    double sum = 0;
    int n = 0;
    for (const CpuCoreSample& c : s.cpus)
        if (c.online && c.load.busy >= 50 && c.freqMHz > 0) {
            sum += c.freqMHz;
            ++n;
        }
    return n ? sum / n : -1;
}

void addHeatSuspects(const Frame& f, Finding& fi, int max)
{
    auto groups = groupByName(f.procs, [](const ProcSample& p) { return std::max(0.0, p.estCpuW) + std::max(0.0, p.estGpuW); }, 0.5);
    double total = 0;
    for (const auto& g : groups)
        total += g.total;
    for (size_t i = 0; i < groups.size() && int(i) < max; ++i)
        fi.suspects.push_back({QStringLiteral("process"), groupLabel(groups[i]), groups[i].top->key.pid,
                               total > 0 ? groups[i].total / total : 0, QObject::tr("≈ %1").arg(fmt::watts(groups[i].total))});
}

void cpuHeat(const RuleContext& ctx)
{
    const Frame& f = ctx.f;
    if (!f.thermal.hasCpuTemp())
        return;
    const double t = f.thermal.cpuTempC;
    const double tj = f.thermal.tjmaxC;
    const double freq = busyCoreAvgFreq(f.sys);
    const double maxFreq = f.sys.policy.hwMaxMHz;

    if (t >= tj - 3) {
        Finding fi;
        fi.id = QStringLiteral("cpu-thermal-limit");
        fi.rule = fi.id;
        fi.severity = Severity::Critical;
        fi.title = QObject::tr("CPU is at its thermal limit (%1 of %2)").arg(fmt::celsius(t), fmt::celsius(tj));
        fi.detail = QObject::tr("The CPU reduces its clocks to stay below Tjmax, so performance drops and frame "
                                "times become uneven.");
        if (freq > 0 && maxFreq > 0)
            fi.evidence << QObject::tr("busy cores average %1 (max boost %2)").arg(fmt::mhz(freq), fmt::mhz(maxFreq));
        if (f.power.packageW > 0)
            fi.evidence << QObject::tr("package power %1").arg(fmt::watts(f.power.packageW));
        addHeatSuspects(f, fi, 5);
        fi.advice = QObject::tr("Check the cooler (fan curve, mounting, paste, dust). To cap heat, lower the power "
                                "limit or disable boost, or limit the heaviest process.");
        ctx.report(fi, 2);
    } else if (t >= ctx.settings.cpuTempWarnC) {
        Finding fi;
        fi.id = QStringLiteral("cpu-hot");
        fi.rule = fi.id;
        fi.severity = Severity::Warning;
        fi.title = QObject::tr("CPU is running hot: %1 (limit %2)").arg(fmt::celsius(t), fmt::celsius(tj));
        fi.detail = QObject::tr("These processes account for most of the load-driven CPU power:");
        if (f.power.packageW > 0)
            fi.evidence << QObject::tr("package power %1 (idle floor %2)").arg(fmt::watts(f.power.packageW), fmt::watts(f.power.idleFloorW));
        addHeatSuspects(f, fi, 5);
        ctx.report(fi, 3);
    }

    // Always-on explanation when the CPU draws well above idle.
    if (f.power.attributableW >= 25 || t >= 70) {
        Finding fi;
        fi.id = QStringLiteral("heat-sources");
        fi.rule = fi.id;
        fi.severity = Severity::Info;
        QStringList top;
        addHeatSuspects(f, fi, 4);
        for (const Suspect& s : fi.suspects)
            top << QStringLiteral("%1 %2").arg(s.label, s.evidence);
        fi.title = QObject::tr("Heat sources: %1").arg(top.isEmpty() ? QObject::tr("no single process stands out") : top.join(QStringLiteral(", ")));
        fi.detail = QObject::tr("CPU %1, package %2 (%3 above idle); GPU %4.")
                        .arg(fmt::celsius(t), fmt::watts(f.power.packageW), fmt::watts(f.power.attributableW),
                             f.gpus.empty() ? QObject::tr("n/a") : QStringLiteral("%1, %2").arg(fmt::celsius(f.gpus[0].tempC), fmt::watts(f.gpus[0].powerW)));
        fi.evidence << QObject::tr("Estimates: CPU power is split by CPU time × clock; GPU power above idle by GPU utilisation.");
        ctx.report(fi, 3);
    }
}

void gpuHeat(const RuleContext& ctx)
{
    for (const GpuSample& g : ctx.f.gpus) {
        if (!g.reasonsValid)
            continue;
        const uint64_t bad = g.eventReasons & (GpuReason::SwThermal | GpuReason::HwThermal | GpuReason::HwSlowdown | GpuReason::HwPowerBrake);
        auto gpuSuspects = [&](Finding& fi) {
            std::vector<const ProcSample*> v;
            for (const ProcSample& p : ctx.f.procs)
                if (p.gpuPct >= 5)
                    v.push_back(&p);
            std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->gpuPct > b->gpuPct; });
            for (size_t i = 0; i < v.size() && i < 4; ++i)
                fi.suspects.push_back(processSuspect(*v[i], v[i]->gpuPct / 100.0, QObject::tr("GPU %1").arg(fmt::percent(v[i]->gpuPct, 0))));
        };
        if (bad) {
            Finding fi;
            fi.id = QStringLiteral("gpu-throttle:%1").arg(g.index);
            fi.rule = QStringLiteral("gpu-throttle");
            fi.severity = (g.eventReasons & (GpuReason::HwThermal | GpuReason::HwSlowdown | GpuReason::HwPowerBrake)) ? Severity::Critical
                                                                                                                     : Severity::Warning;
            fi.title = QObject::tr("GPU %1 is throttling: %2").arg(g.index).arg(decodeGpuEventReasons(bad).join(QStringLiteral(", ")));
            fi.detail = QObject::tr("%1 at %2 (slowdown %3), %4 of %5, SM clock %6 of %7 MHz.")
                            .arg(g.name, fmt::celsius(g.tempC), fmt::celsius(g.slowdownC), fmt::watts(g.powerW), fmt::watts(g.powerLimitW))
                            .arg(g.clockSmMHz)
                            .arg(g.clockSmMaxMHz);
            gpuSuspects(fi);
            fi.advice = (bad & GpuReason::HwPowerBrake)
                            ? QObject::tr("HW power brake is asserted by the board/PSU: check PCIe power cables and the PSU.")
                            : QObject::tr("Improve case airflow / GPU fan curve, or cap the frame rate / power limit.");
            ctx.report(fi, 2);
        } else if (g.slowdownC > 0 && g.tempC >= g.slowdownC - 5) {
            Finding fi;
            fi.id = QStringLiteral("gpu-hot:%1").arg(g.index);
            fi.rule = QStringLiteral("gpu-hot");
            fi.severity = Severity::Warning;
            fi.title = QObject::tr("GPU %1 is close to its slowdown temperature (%2 of %3)").arg(g.index).arg(fmt::celsius(g.tempC), fmt::celsius(g.slowdownC));
            gpuSuspects(fi);
            ctx.report(fi, 3);
        } else if ((g.eventReasons & GpuReason::SwPowerCap) && g.utilGpu >= 80) {
            Finding fi;
            fi.id = QStringLiteral("gpu-powercap:%1").arg(g.index);
            fi.rule = QStringLiteral("gpu-powercap");
            fi.severity = Severity::Info;
            fi.title = QObject::tr("GPU %1 is power-limited at %2 (normal at full load)").arg(g.index).arg(fmt::watts(g.powerW));
            gpuSuspects(fi);
            ctx.report(fi, 5);
        }
    }
}

void fans(const RuleContext& ctx)
{
    const Frame& f = ctx.f;
    for (const SensorReading& r : f.sensors) {
        if (r.kind != SensorKind::Fan)
            continue;
        if (r.value > 100) {
            ctx.st.fansSeenSpinning.insert(r.key);
            continue;
        }
        // A header that never spun is probably unconnected; one that stopped is suspicious when hot.
        if (!ctx.st.fansSeenSpinning.contains(r.key) || !f.thermal.hasCpuTemp() || f.thermal.cpuTempC < 65)
            continue;
        Finding fi;
        fi.id = QStringLiteral("fan-stopped:") + r.key;
        fi.rule = QStringLiteral("fan-stopped");
        fi.severity = Severity::Warning;
        fi.title = QObject::tr("Fan %1 %2 stopped while the CPU is at %3").arg(r.chip, r.label, fmt::celsius(f.thermal.cpuTempC));
        fi.detail = QObject::tr("It was spinning earlier in this session. A zero-RPM fan mode is normal at low "
                                "temperatures, but not while the CPU is this warm.");
        fi.suspects.push_back({QStringLiteral("device"), r.chip + QLatin1Char(' ') + r.label, 0, 1.0, QObject::tr("0 RPM")});
        ctx.report(fi, 3);
    }
}

void powerSettings(const RuleContext& ctx)
{
    const CpuPolicy& p = ctx.f.sys.policy;
    if (p.epp == QLatin1String("power")) {
        Finding fi;
        fi.id = QStringLiteral("epp-power");
        fi.rule = fi.id;
        fi.severity = Severity::Info;
        fi.title = QObject::tr("CPU energy preference is \"power\"");
        fi.detail = QObject::tr("The CPU ramps its clock slowly and stays low; short bursts of work (frames, UI) take "
                                "longer, which can show up as micro-stutter.");
        fi.advice = QObject::tr("Use balance_performance or performance (power profile in the desktop settings, or "
                                "`powerprofilesctl set balanced`).");
        ctx.report(fi, 3);
    }
    if (p.boost == 0) {
        Finding fi;
        fi.id = QStringLiteral("boost-off");
        fi.rule = fi.id;
        fi.severity = Severity::Info;
        fi.title = QObject::tr("CPU boost is disabled");
        fi.detail = QObject::tr("Clocks are capped at base frequency: cooler and quieter, but slower single-thread work.");
        ctx.report(fi, 3);
    }
    if (p.hwMaxMHz > 0 && p.scalingMaxMHz > 0 && p.scalingMaxMHz < p.hwMaxMHz * 0.95) {
        Finding fi;
        fi.id = QStringLiteral("freq-capped");
        fi.rule = fi.id;
        fi.severity = Severity::Info;
        fi.title = QObject::tr("CPU frequency is capped at %1 (hardware max %2)").arg(fmt::mhz(p.scalingMaxMHz), fmt::mhz(p.hwMaxMHz));
        fi.detail = QObject::tr("scaling_max_freq has been lowered, e.g. by a power profile, TLP or thermald.");
        ctx.report(fi, 3);
    }
    if (p.governor == QLatin1String("powersave") && !p.driver.contains(QLatin1String("pstate"))) {
        Finding fi;
        fi.id = QStringLiteral("governor-powersave");
        fi.rule = fi.id;
        fi.severity = Severity::Warning;
        fi.title = QObject::tr("CPU governor \"powersave\" with %1 pins the clock at minimum").arg(p.driver);
        fi.advice = QObject::tr("Use schedutil or ondemand with this driver.");
        ctx.report(fi, 3);
    }
}

} // namespace

void thermalRules(const RuleContext& ctx)
{
    cpuHeat(ctx);
    gpuHeat(ctx);
    fans(ctx);
    powerSettings(ctx);
}

} // namespace culprit
