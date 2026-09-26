// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

// CPU load diagnosis: who is using the CPUs, who is waiting for them, and why
// the load average is what it is.

#include "core/analysis/rules/Rules.h"

#include "core/Format.h"

#include <algorithm>
#include <map>

namespace culprit {

namespace {

void highCpuProcesses(const RuleContext& ctx)
{
    const Frame& f = ctx.f;
    const double threshold = ctx.settings.highCpuCores * 100.0;
    std::unordered_map<ProcKey, int, ProcKeyHash> next;
    for (const ProcSample& p : f.procs) {
        if (p.cpuPct < threshold || p.kernelThread)
            continue;
        const int secs = ctx.st.highCpuSecs[p.key] + std::max(1, int(f.sys.intervalSec + 0.5));
        next[p.key] = secs;
        if (secs < ctx.settings.highCpuSustainSec)
            continue;
        const double cores = p.cpuPct / 100.0;
        const bool heavy = cores >= 2.0 || cores >= f.sys.onlineCpus * 0.25;
        Finding fi;
        fi.id = QStringLiteral("high-cpu:%1:%2").arg(p.key.pid).arg(p.key.starttime);
        fi.rule = QStringLiteral("high-cpu");
        fi.severity = heavy ? Severity::Warning : Severity::Info;
        fi.title = QObject::tr("%1 is using %2 CPU core(s)").arg(procName(p)).arg(cores, 0, 'f', 1);
        fi.detail = QObject::tr("Sustained for %1 across %2 thread(s).").arg(fmt::duration(secs)).arg(p.threads);
        fi.evidence << QObject::tr("CPU %1 of one core (%2 of the whole system)")
                           .arg(fmt::percent(p.cpuPct), fmt::percent(p.cpuPct / std::max(1, f.sys.onlineCpus)));
        if (p.estCpuW > 0)
            fi.evidence << QObject::tr("≈ %1 of CPU power attributed").arg(fmt::watts(p.estCpuW));
        if (!p.unit.isEmpty())
            fi.evidence << QObject::tr("unit: %1").arg(p.unit);
        fi.suspects.push_back(processSuspect(p, 1.0, fmt::percent(p.cpuPct)));
        fi.advice = QObject::tr("If this is unexpected, inspect it in the Processes tab. To keep it from starving "
                                "interactive apps, lower its priority (nice/ionice) or run it under "
                                "`systemd-run --user -p CPUQuota=…`.");
        ctx.report(fi, 1);
    }
    ctx.st.highCpuSecs = std::move(next);

    // Many processes of one app (browsers, electron apps) adding up.
    QHash<QString, std::pair<double, int>> byUnit;
    for (const ProcSample& p : f.procs)
        if (!p.unit.isEmpty() && !p.kernelThread) {
            auto& e = byUnit[p.unit];
            e.first += p.cpuPct;
            e.second += 1;
        }
    QHash<QString, int> nextUnit;
    for (auto it = byUnit.begin(); it != byUnit.end(); ++it) {
        const double pct = it.value().first;
        if (pct < threshold * 1.5 || it.value().second < 3)
            continue;
        const ProcSample* top = nullptr;
        for (const ProcSample& p : f.procs)
            if (p.unit == it.key() && (!top || p.cpuPct > top->cpuPct))
                top = &p;
        if (top && top->cpuPct > pct * 0.7)
            continue;   // one process dominates: the per-process rule covers it
        const int secs = ctx.st.unitHighCpuSecs.value(it.key()) + 1;
        nextUnit[it.key()] = secs;
        if (secs < ctx.settings.highCpuSustainSec)
            continue;
        Finding fi;
        fi.id = QStringLiteral("high-cpu-unit:") + it.key();
        fi.rule = QStringLiteral("high-cpu-unit");
        fi.severity = pct >= 200 ? Severity::Warning : Severity::Info;
        fi.title = QObject::tr("%1 uses %2 CPU cores across %3 processes").arg(it.key()).arg(pct / 100.0, 0, 'f', 1).arg(it.value().second);
        fi.detail = QObject::tr("No single process dominates; the load is spread over the app's processes.");
        for (const ProcSample& p : f.procs)
            if (p.unit == it.key() && p.cpuPct >= 5)
                fi.suspects.push_back(processSuspect(p, p.cpuPct / pct, fmt::percent(p.cpuPct)));
        std::sort(fi.suspects.begin(), fi.suspects.end(), [](const Suspect& a, const Suspect& b) { return a.score > b.score; });
        if (fi.suspects.size() > 6)
            fi.suspects.resize(6);
        ctx.report(fi, 1);
    }
    ctx.st.unitHighCpuSecs = nextUnit;
}

void runQueue(const RuleContext& ctx)
{
    const Frame& f = ctx.f;
    const double waiting = f.sys.runDelayTotalPct / 100.0;   // avg runnable tasks waiting for a CPU
    // Oversubscribed = tasks queue *and* the machine as a whole is busy. Queueing
    // with idle CPUs elsewhere is a hotspot (affinity / pinning), handled below.
    const bool busy = f.sys.total.busy >= 75;
    ctx.st.runqSecs = waiting >= 1.0 && busy ? ctx.st.runqSecs + 1 : 0;
    if (ctx.st.runqSecs >= 3) {
        Finding fi;
        fi.id = QStringLiteral("cpu-saturated");
        fi.rule = fi.id;
        fi.severity = waiting >= f.sys.onlineCpus * 0.5 ? Severity::Critical : Severity::Warning;
        fi.title = QObject::tr("CPU oversubscribed: on average %1 runnable tasks are waiting for a CPU").arg(waiting, 0, 'f', 1);
        fi.detail = QObject::tr("Tasks that are ready to run are queueing behind others. Anything latency-sensitive "
                                "(games, audio, UI) will stutter while this lasts.");
        fi.evidence << QObject::tr("%1 CPUs busy %2, %3 tasks running right now")
                           .arg(f.sys.onlineCpus)
                           .arg(fmt::percent(f.sys.total.busy))
                           .arg(f.sys.procsRunning);
        // Causes: biggest CPU consumers. Victims: most delayed.
        auto groups = groupByName(f.procs, [](const ProcSample& p) { return p.cpuPct; }, 20);
        for (size_t i = 0; i < groups.size() && i < 5; ++i)
            fi.suspects.push_back({QStringLiteral("process"), groupLabel(groups[i]), groups[i].top->key.pid,
                                   std::min(1.0, groups[i].total / std::max(1.0, double(f.sys.total.busy) * f.sys.onlineCpus)),
                                   QObject::tr("using %1 of a core").arg(fmt::percent(groups[i].total))});
        std::vector<const ProcSample*> victims;
        for (const ProcSample& p : f.procs)
            if (p.runDelayMsPs >= 50)
                victims.push_back(&p);
        std::sort(victims.begin(), victims.end(), [](auto* a, auto* b) { return a->runDelayMsPs > b->runDelayMsPs; });
        for (size_t i = 0; i < victims.size() && i < 5; ++i)
            fi.evidence << QObject::tr("%1 waited %2 per second for a CPU").arg(procName(*victims[i]), fmt::ms(victims[i]->runDelayMsPs));
        fi.advice = QObject::tr("Reduce parallelism of the heaviest job (e.g. make -j, compressor threads), renice it, "
                                "or confine it with `systemd-run -p AllowedCPUs=…`/CPUQuota so interactive work keeps "
                                "free cores.");
        ctx.report(fi, 1);
    }

    // One CPU congested while the system as a whole isn't: pinning / affinity / IRQ steering.
    const size_t n = f.sys.cpus.size();
    ctx.st.cpuHotspotSecs.resize(n, 0);
    // CFS quota throttling also shows up as run-queue wait; don't call that a hotspot.
    std::vector<bool> throttledCpu(n, false);
    for (const CgroupSample& cg : f.cgroups)
        if (cg.throttledPct >= 10)
            for (const ProcSample& p : f.procs)
                if (p.lastCpu >= 0 && size_t(p.lastCpu) < n && p.cpuPct > 1 && p.cgroup.startsWith(cg.path))
                    throttledCpu[size_t(p.lastCpu)] = true;
    if (!busy) {
        for (size_t c = 0; c < n; ++c) {
            const CpuCoreSample& cs = f.sys.cpus[c];
            ctx.st.cpuHotspotSecs[c] = cs.runDelayPct >= 40 && !throttledCpu[c] ? ctx.st.cpuHotspotSecs[c] + 1 : 0;
            if (ctx.st.cpuHotspotSecs[c] < 3)
                continue;
            Finding fi;
            fi.id = QStringLiteral("cpu-hotspot:%1").arg(c);
            fi.rule = QStringLiteral("cpu-hotspot");
            fi.severity = Severity::Warning;
            fi.title = QObject::tr("CPU %1 is congested while other CPUs are free").arg(c);
            fi.detail = QObject::tr("Tasks queue up on CPU %1 (%2 of the time a task was waiting) even though the "
                                    "system isn't saturated. Typically caused by CPU affinity/pinning (taskset, "
                                    "isolcpus, containers), or IRQ/softirq work steered to one CPU.")
                            .arg(c)
                            .arg(fmt::percent(cs.runDelayPct, 0));
            fi.evidence << QObject::tr("CPU %1: busy %2, irq+softirq %3").arg(c).arg(fmt::percent(cs.load.busy), fmt::percent(cs.load.irq + cs.load.softirq));
            for (const ProcSample& p : f.procs)
                if (p.lastCpu == int(c) && p.cpuPct >= 10)
                    fi.suspects.push_back(processSuspect(p, std::min(1.0, p.cpuPct / 100.0),
                                                         QObject::tr("%1 on CPU %2").arg(fmt::percent(p.cpuPct)).arg(c)));
            std::sort(fi.suspects.begin(), fi.suspects.end(), [](const Suspect& a, const Suspect& b) { return a.score > b.score; });
            fi.advice = QObject::tr("Check the affinity of the suspects (Processes → CPU affinity) and IRQ affinity "
                                    "(/proc/irq/*/smp_affinity_list).");
            ctx.report(fi, 1);
        }
    }
}

void loadExplained(const RuleContext& ctx)
{
    const Frame& f = ctx.f;
    const SystemSample& s = f.sys;
    // Smooth the instantaneous R/D counts (or use the 50 Hz averages when present).
    const double r = s.avgRunnable >= 0 ? s.avgRunnable : double(s.procsRunning);
    const double d = s.avgBlocked >= 0 ? s.avgBlocked : double(s.procsBlocked);
    auto ewma = [](double& e, double v) { e = e < 0 ? v : e * 0.85 + v * 0.15; };
    ewma(ctx.st.runnableEwma, r);
    ewma(ctx.st.blockedEwma, d);

    if (s.load1 < std::max(2.0, s.onlineCpus * 0.5))
        return;
    const double R = ctx.st.runnableEwma, D = ctx.st.blockedEwma;
    const bool ioDriven = D > R && D >= 2;
    Finding fi;
    fi.id = QStringLiteral("load-explained");
    fi.rule = fi.id;
    fi.severity = ioDriven ? Severity::Warning : Severity::Info;
    fi.title = ioDriven ? QObject::tr("Load %1 is mostly blocked I/O, not CPU (≈%2 threads in D state)").arg(s.load1, 0, 'f', 1).arg(D, 0, 'f', 1)
                        : QObject::tr("Load %1 on %2 CPUs: ≈%3 running, ≈%4 blocked").arg(s.load1, 0, 'f', 1).arg(s.onlineCpus).arg(R, 0, 'f', 1).arg(D, 0, 'f', 1);
    fi.detail = QObject::tr("Linux load average counts runnable tasks and tasks in uninterruptible sleep (usually "
                            "waiting on disk, NFS or a driver). CPU busy right now: %1.")
                    .arg(fmt::percent(s.total.busy));
    // Group D threads by process + wait channel.
    std::map<QString, std::pair<int, const DStateThread*>> groups;
    for (const DStateThread& t : f.dstate) {
        const QString key = t.procComm + QLatin1Char('|') + (t.wchan.isEmpty() ? QObject::tr("unknown wait") : t.wchan);
        auto& g = groups[key];
        g.first++;
        g.second = &t;
    }
    for (const auto& [key, g] : groups) {
        const DStateThread& t = *g.second;
        fi.evidence << QObject::tr("%1 thread(s) of %2 (pid %3) blocked in %4")
                           .arg(g.first)
                           .arg(t.procComm)
                           .arg(t.pid)
                           .arg(t.wchan.isEmpty() ? QObject::tr("an unknown kernel wait (enable Deep trace to see it)") : t.wchan);
        fi.suspects.push_back({QStringLiteral("process"), QStringLiteral("%1 (%2)").arg(t.procComm).arg(t.pid), t.pid,
                               double(g.first) / std::max<size_t>(1, f.dstate.size()), QObject::tr("%1 in D state").arg(g.first)});
    }
    if (!ioDriven) {
        auto top = groupByName(f.procs, [](const ProcSample& p) { return p.cpuPct; }, 25);
        for (size_t i = 0; i < top.size() && i < 4; ++i)
            fi.suspects.push_back({QStringLiteral("process"), groupLabel(top[i]), top[i].top->key.pid, top[i].total / 100.0 / std::max(1.0, R),
                                   QObject::tr("%1 of a core").arg(fmt::percent(top[i].total))});
    }
    if (ioDriven)
        fi.advice = QObject::tr("High load with idle CPUs means something is stuck waiting on I/O. Look at the "
                                "Load & I/O tab: blocked threads, disk utilisation and await.");
    ctx.report(fi, 3);
}

void realtimeHogs(const RuleContext& ctx)
{
    for (const ProcSample& p : ctx.f.procs) {
        if ((p.policy != 1 && p.policy != 2) || p.cpuPct < 40 || p.kernelThread)
            continue;
        Finding fi;
        fi.id = QStringLiteral("rt-hog:%1:%2").arg(p.key.pid).arg(p.key.starttime);
        fi.rule = QStringLiteral("rt-hog");
        fi.severity = Severity::Warning;
        fi.title = QObject::tr("Real-time task %1 is using %2 of a CPU").arg(procName(p), fmt::percent(p.cpuPct, 0));
        fi.detail = QObject::tr("SCHED_%1 priority %2 preempts every normal task on its CPU. A busy real-time task "
                                "causes stutter for everything scheduled behind it.")
                        .arg(p.policy == 1 ? QStringLiteral("FIFO") : QStringLiteral("RR"))
                        .arg(p.rtPrio);
        fi.suspects.push_back(processSuspect(p, 1.0, QObject::tr("RT prio %1").arg(p.rtPrio)));
        fi.advice = QObject::tr("Unless this is an audio/latency-critical service, run it with a normal policy "
                                "(`chrt -o -p 0 %1`).").arg(p.key.pid);
        ctx.report(fi, 2);
    }
}

} // namespace

void cpuRules(const RuleContext& ctx)
{
    highCpuProcesses(ctx);
    runQueue(ctx);
    loadExplained(ctx);
    realtimeHogs(ctx);
}

} // namespace culprit
