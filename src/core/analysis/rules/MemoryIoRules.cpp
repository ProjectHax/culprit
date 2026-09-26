// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

// Memory pressure, storage, cgroup limits and interrupt storms.

#include "core/analysis/rules/Rules.h"

#include "core/Format.h"

#include <algorithm>

namespace culprit {

namespace {

void memory(const RuleContext& ctx)
{
    const Frame& f = ctx.f;
    const SystemSample& s = f.sys;
    // Processes stalling on memory (major faults) first — they are what the user
    // feels — then the biggest resident sets, which are what is squeezing memory.
    auto memSuspects = [&](Finding& fi) {
        std::vector<const ProcSample*> faulting;
        for (const ProcSample& p : f.procs)
            if (p.majfltPs >= 20)
                faulting.push_back(&p);
        std::sort(faulting.begin(), faulting.end(), [](auto* a, auto* b) { return a->majfltPs > b->majfltPs; });
        double maxFaults = faulting.empty() ? 1 : faulting.front()->majfltPs;
        for (size_t i = 0; i < faulting.size() && i < 3; ++i)
            fi.suspects.push_back(processSuspect(*faulting[i], faulting[i]->majfltPs / maxFaults,
                                                 QObject::tr("%1 major faults/s — stalled on disk/swap").arg(fmt::count(faulting[i]->majfltPs))));
        auto groups = groupByName(f.procs, [](const ProcSample& p) { return double(p.rssBytes); }, 64.0 * 1024 * 1024);
        for (size_t i = 0; i < groups.size() && i < 4; ++i)
            fi.suspects.push_back({QStringLiteral("process"), groupLabel(groups[i]), groups[i].top->key.pid,
                                   0.5 * groups[i].total / std::max(1.0, double(s.mem.totalKb) * 1024.0),
                                   QObject::tr("%1 resident").arg(fmt::bytes(groups[i].total))});
    };

    if (s.vm.oomKill > 0) {
        Finding fi;
        fi.id = QStringLiteral("oom-kill");
        fi.rule = fi.id;
        fi.severity = Severity::Critical;
        fi.title = QObject::tr("The kernel OOM killer terminated a process");
        fi.detail = QObject::tr("Memory ran out completely. `journalctl -k | grep -i oom` shows which process was killed.");
        memSuspects(fi);
        ctx.report(fi, 1);
    }

    const bool reclaim = s.vm.pgscanDirect > 0 || s.vm.allocstall > 0 || s.vm.compactStall > 0;
    ctx.st.reclaimSecs = reclaim ? ctx.st.reclaimSecs + 1 : 0;
    if (ctx.st.reclaimSecs >= 2) {
        Finding fi;
        fi.id = QStringLiteral("direct-reclaim");
        fi.rule = fi.id;
        fi.severity = Severity::Warning;
        fi.title = s.vm.compactStall > 0 && s.vm.pgscanDirect == 0
                       ? QObject::tr("Memory allocations are stalling in compaction")
                       : QObject::tr("Memory allocations are stalling in direct reclaim");
        fi.detail = QObject::tr("Programs asking for memory have to free pages themselves before continuing — a "
                                "classic cause of multi-millisecond hitches.");
        fi.evidence << QObject::tr("direct scan %1 pages/s, allocation stalls %2/s, compaction stalls %3/s")
                           .arg(fmt::count(s.vm.pgscanDirect), fmt::count(s.vm.allocstall), fmt::count(s.vm.compactStall));
        fi.evidence << QObject::tr("available memory %1 of %2").arg(fmt::kb(s.mem.availableKb), fmt::kb(s.mem.totalKb));
        memSuspects(fi);
        fi.advice = QObject::tr("Free memory (close the largest consumers), add swap/zram, or raise vm.min_free_kbytes. "
                                "Compaction stalls with plenty of free memory point at transparent hugepages: try "
                                "`echo defer+madvise > /sys/kernel/mm/transparent_hugepage/defrag`.");
        ctx.report(fi, 1);
    }

    if (s.mem.totalKb && double(s.mem.availableKb) < double(s.mem.totalKb) * 0.05) {
        Finding fi;
        fi.id = QStringLiteral("low-memory");
        fi.rule = fi.id;
        fi.severity = Severity::Warning;
        fi.title = QObject::tr("Low memory: only %1 available of %2").arg(fmt::kb(s.mem.availableKb), fmt::kb(s.mem.totalKb));
        memSuspects(fi);
        ctx.report(fi, 3);
    }

    if (s.vm.pswpin >= 100) {
        Finding fi;
        fi.id = QStringLiteral("swapping");
        fi.rule = fi.id;
        fi.severity = s.vm.pswpin >= 1000 ? Severity::Critical : Severity::Warning;
        fi.title = QObject::tr("Swapping in %1 pages/s").arg(fmt::count(s.vm.pswpin));
        fi.detail = QObject::tr("Processes are waiting for memory to be read back from swap.");
        memSuspects(fi);
        ctx.report(fi, 2);
    }
}

void disks(const RuleContext& ctx)
{
    QHash<QString, int> next;
    for (const DiskSample& d : ctx.f.sys.disks) {
        const bool busy = d.utilPct >= 95 || (d.awaitMs >= 100 && d.readsPs + d.writesPs >= 5);
        const int secs = busy ? ctx.st.diskBusySecs.value(d.name) + 1 : 0;
        if (secs)
            next[d.name] = secs;
        if (secs < 5)
            continue;
        Finding fi;
        fi.id = QStringLiteral("disk-busy:") + d.name;
        fi.rule = QStringLiteral("disk-busy");
        fi.severity = d.awaitMs >= 500 ? Severity::Critical : Severity::Warning;
        fi.title = d.utilPct >= 95 ? QObject::tr("%1 is saturated (%2 busy, %3 average wait)").arg(d.label, fmt::percent(d.utilPct, 0), fmt::ms(d.awaitMs))
                                   : QObject::tr("%1 is slow: %2 average I/O wait").arg(d.label, fmt::ms(d.awaitMs));
        fi.evidence << QObject::tr("read %1 (%2 IOPS), write %3 (%4 IOPS), %5 in flight")
                           .arg(fmt::bytesPerSec(d.readBps), fmt::count(d.readsPs), fmt::bytesPerSec(d.writeBps), fmt::count(d.writesPs))
                           .arg(d.inFlight);
        std::vector<const ProcSample*> io;
        for (const ProcSample& p : ctx.f.procs)
            if (p.ioReadBps + p.ioWriteBps >= 256 * 1024)
                io.push_back(&p);
        std::sort(io.begin(), io.end(), [](auto* a, auto* b) { return a->ioReadBps + a->ioWriteBps > b->ioReadBps + b->ioWriteBps; });
        for (size_t i = 0; i < io.size() && i < 4; ++i)
            fi.suspects.push_back(processSuspect(*io[i], 1.0 / double(i + 1),
                                                 QObject::tr("read %1, write %2").arg(fmt::bytesPerSec(io[i]->ioReadBps), fmt::bytesPerSec(io[i]->ioWriteBps))));
        if (io.empty())
            fi.evidence << QObject::tr("Per-process I/O of other users' processes needs Deep trace (root).");
        fi.advice = QObject::tr("Lower the I/O priority of the heaviest writer (Processes → I/O priority → Idle).");
        ctx.report(fi, 1);
    }
    ctx.st.diskBusySecs = next;

    for (const DStateThread& t : ctx.f.dstate) {
        if (t.ageSec < 10)
            continue;
        Finding fi;
        fi.id = QStringLiteral("dstate-stuck:%1").arg(t.tid);
        fi.rule = QStringLiteral("dstate-stuck");
        fi.severity = t.ageSec >= 120 ? Severity::Critical : Severity::Warning;
        fi.title = QObject::tr("%1 (pid %2) has been stuck in uninterruptible sleep for %3")
                       .arg(t.procComm)
                       .arg(t.pid)
                       .arg(fmt::duration(t.ageSec));
        fi.detail = QObject::tr("Thread %1 (%2) is blocked in the kernel%3. Long D-state usually means a hung "
                                "storage device, NFS/network filesystem or a driver problem.")
                        .arg(t.comm)
                        .arg(t.tid)
                        .arg(t.wchan.isEmpty() ? QString() : QObject::tr(" at %1").arg(t.wchan));
        for (const QString& frame : t.stack)
            fi.evidence << frame;
        fi.suspects.push_back({QStringLiteral("thread"), QStringLiteral("%1/%2").arg(t.procComm, t.comm), t.pid, 1.0, t.wchan});
        ctx.report(fi, 1);
    }
}

void cgroups(const RuleContext& ctx)
{
    for (const CgroupSample& c : ctx.f.cgroups) {
        if (c.throttledPct >= 10 && c.throttledMsPs >= 20) {
            Finding fi;
            fi.id = QStringLiteral("cgroup-cpu:") + c.path;
            fi.rule = QStringLiteral("cgroup-cpu-throttle");
            fi.severity = Severity::Warning;
            fi.title = QObject::tr("%1 is throttled by its CPU quota (%2 of periods)").arg(c.unit, fmt::percent(c.throttledPct, 0));
            fi.detail = QObject::tr("Its tasks are stopped for %1 per second once the quota%2 is used up — they "
                                    "stall until the next 100 ms period.")
                            .arg(fmt::ms(c.throttledMsPs))
                            .arg(c.cpuQuotaCores > 0 ? QObject::tr(" (%1 cores)").arg(c.cpuQuotaCores, 0, 'f', 2) : QString());
            fi.evidence << QObject::tr("cgroup %1").arg(c.path);
            for (const ProcSample& p : ctx.f.procs)
                if (p.cgroup.startsWith(c.path) && p.cpuPct >= 5)
                    fi.suspects.push_back(processSuspect(p, 0.5, fmt::percent(p.cpuPct)));
            fi.advice = QObject::tr("Raise or remove CPUQuota for this unit (`systemctl set-property %1 CPUQuota=`).").arg(c.unit);
            ctx.report(fi, 2);
        }
        if (c.memHighEvents > 0 || c.memMaxEvents > 0 || c.oomKills > 0) {
            Finding fi;
            fi.id = QStringLiteral("cgroup-mem:") + c.path;
            fi.rule = QStringLiteral("cgroup-mem");
            fi.severity = c.oomKills ? Severity::Critical : Severity::Warning;
            fi.title = c.oomKills ? QObject::tr("%1 hit its memory limit: OOM kill").arg(c.unit)
                                  : QObject::tr("%1 is being throttled at its memory.high limit").arg(c.unit);
            fi.detail = QObject::tr("Tasks in this cgroup are forced into memory reclaim (and slowed down) whenever it "
                                    "exceeds its limit%1.")
                            .arg(c.memHighBytes > 0 ? QObject::tr(" of %1").arg(fmt::bytes(c.memHighBytes)) : QString());
            fi.evidence << QObject::tr("memory.events.local: high +%1, max +%2, oom_kill +%3").arg(c.memHighEvents).arg(c.memMaxEvents).arg(c.oomKills);
            fi.evidence << QObject::tr("cgroup %1").arg(c.path);
            for (const ProcSample& p : ctx.f.procs)
                if (p.cgroup.startsWith(c.path) && (p.majfltPs >= 1 || p.cpuPct >= 1))
                    fi.suspects.push_back(processSuspect(p, 0.8,
                                                         QObject::tr("%1 resident, %2 major faults/s").arg(fmt::bytes(double(p.rssBytes)), fmt::count(p.majfltPs))));
            fi.advice = QObject::tr("Raise MemoryHigh for this unit or reduce the workload's memory use.");
            ctx.report(fi, 1);
        }
    }
}

void interrupts(const RuleContext& ctx)
{
    const SystemSample& s = ctx.f.sys;
    QHash<QString, int> next;
    for (const IrqRate& irq : s.irqs) {
        double& base = ctx.st.irqBaseline[irq.label];
        const double prevBase = base;
        base = base <= 0 ? irq.perSec : base * 0.98 + irq.perSec * 0.02;
        // Timer/IPI lines scale with load; judge device IRQs.
        bool numeric = false;
        irq.label.toInt(&numeric);
        if (!numeric || ctx.st.evaluations < 30)
            continue;
        const bool storm = irq.perSec >= 20000 && irq.perSec >= prevBase * 5;
        const int secs = storm ? ctx.st.irqHotSecs.value(irq.label) + 1 : 0;
        if (secs)
            next[irq.label] = secs;
        if (secs < 3)
            continue;
        Finding fi;
        fi.id = QStringLiteral("irq-storm:") + irq.label;
        fi.rule = QStringLiteral("irq-storm");
        fi.severity = Severity::Warning;
        fi.title = QObject::tr("Interrupt storm: IRQ %1 (%2) at %3").arg(irq.label, irq.name, fmt::rate(irq.perSec));
        fi.detail = QObject::tr("About %1× its usual rate, mostly on CPU %2. Every interrupt steals time from the task "
                                "running on that CPU.")
                        .arg(irq.perSec / std::max(1.0, prevBase), 0, 'f', 0)
                        .arg(irq.topCpu);
        fi.suspects.push_back({QStringLiteral("irq"), QStringLiteral("IRQ %1 %2").arg(irq.label, irq.name), 0, 1.0, fmt::rate(irq.perSec)});
        fi.advice = QObject::tr("Check the device/driver (dmesg). For NICs, interrupt coalescing (`ethtool -C`) "
                                "reduces the rate.");
        ctx.report(fi, 1);
    }
    ctx.st.irqHotSecs = next;

    // A CPU losing a big share of its time to interrupt handling.
    ctx.st.irqCpuSecs.resize(s.cpus.size(), 0);
    for (size_t c = 0; c < s.cpus.size(); ++c) {
        const double pct = s.cpus[c].load.irq + s.cpus[c].load.softirq;
        ctx.st.irqCpuSecs[c] = pct >= 25 ? ctx.st.irqCpuSecs[c] + 1 : 0;
        if (ctx.st.irqCpuSecs[c] < 3)
            continue;
        Finding fi;
        fi.id = QStringLiteral("irq-cpu:%1").arg(c);
        fi.rule = QStringLiteral("irq-cpu");
        fi.severity = Severity::Warning;
        fi.title = QObject::tr("CPU %1 spends %2 of its time handling interrupts").arg(c).arg(fmt::percent(pct, 0));
        for (const IrqRate& irq : s.irqs)
            if (irq.topCpu == int(c) && irq.topCpuPerSec >= 1000)
                fi.suspects.push_back({QStringLiteral("irq"), QStringLiteral("IRQ %1 %2").arg(irq.label, irq.name), 0,
                                       std::min(1.0, irq.topCpuPerSec / 50000.0), fmt::rate(irq.topCpuPerSec)});
        for (const SoftirqRate& si : s.softirqs)
            if (si.topCpu == int(c) && si.topCpuPerSec >= 1000)
                fi.suspects.push_back({QStringLiteral("softirq"), si.name, 0, std::min(1.0, si.topCpuPerSec / 50000.0), fmt::rate(si.topCpuPerSec)});
        fi.advice = QObject::tr("Spread the device's interrupts over more CPUs (irqbalance, RSS/RPS for NICs) or move "
                                "latency-sensitive work off CPU %1.").arg(c);
        ctx.report(fi, 1);
    }
}

} // namespace

void memoryIoRules(const RuleContext& ctx)
{
    memory(ctx);
    disks(ctx);
    cgroups(ctx);
    interrupts(ctx);
}

} // namespace culprit
