// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/report/TextReport.h"

#include "core/Format.h"

#include <QTextStream>

#include <algorithm>

namespace culprit {

namespace {

QString pad(const QString& s, int w) { return s.leftJustified(w, QLatin1Char(' '), true); }
QString rpad(const QString& s, int w) { return s.rightJustified(w, QLatin1Char(' '), true); }

void section(QTextStream& o, const QString& title)
{
    o << "\n== " << title << " " << QString(std::max(0, 72 - int(title.size())), QLatin1Char('=')) << "\n";
}

} // namespace

QString findingText(const Finding& f)
{
    QString s;
    QTextStream o(&s);
    o << "[" << severityName(f.severity).toUpper() << "] " << f.title << (f.active ? "" : "  (resolved)") << "\n";
    if (!f.detail.isEmpty())
        o << "    " << f.detail << "\n";
    for (const QString& e : f.evidence)
        o << "    - " << e << "\n";
    for (const Suspect& su : f.suspects)
        o << "    suspect: " << su.label << (su.evidence.isEmpty() ? QString() : QStringLiteral(" — ") + su.evidence) << "\n";
    if (!f.advice.isEmpty())
        o << "    advice: " << f.advice << "\n";
    return s;
}

QString hitchText(const Hitch& h)
{
    QString s;
    QTextStream o(&s);
    o << fmt::wallTime(h.wallMs) << "  " << fmt::ms(h.maxOvershootMs) << " late  [" << hitchClassName(h.cls) << "]"
      << (h.global ? "  global" : "") << (h.periodSec > 0 ? QStringLiteral("  periodic %1 s").arg(h.periodSec, 0, 'f', 2) : QString())
      << "\n";
    if (!h.summary.isEmpty())
        o << "    " << h.summary << "\n";
    QStringList cpus;
    for (int c : h.cpus)
        cpus << QString::number(c);
    o << "    CPUs: " << cpus.join(QLatin1Char(',')) << "  run-queue share of delay: "
      << fmt::percent(h.runDelayShare * 100.0, 0) << "\n";
    for (const QString& e : h.evidence)
        o << "    - " << e << "\n";
    for (const Suspect& su : h.suspects)
        o << "    suspect (" << QString::number(su.score, 'f', 2) << "): " << su.label
          << (su.evidence.isEmpty() ? QString() : QStringLiteral(" — ") + su.evidence) << "\n";
    if (h.deepResolved) {
        o << "    deep trace — CPU occupancy during the stall:\n";
        for (const DeepBlocker& b : h.blockers)
            o << "      " << pad(b.kind, 8) << pad(b.name, 24) << (b.pid ? QStringLiteral("pid %1 ").arg(b.pid) : QString())
              << fmt::ms(b.ms) << "\n";
    }
    return s;
}

QString frameText(const Frame& f, const std::vector<Hitch>& hitches)
{
    QString s;
    QTextStream o(&s);
    const SystemSample& y = f.sys;

    o << "Culprit snapshot  " << fmt::wallTime(f.wallMs) << "  (uptime " << fmt::duration(y.uptimeSec) << ")\n";

    section(o, QStringLiteral("Findings"));
    int active = 0;
    for (const Finding& fi : f.findings) {
        if (!fi.active)
            continue;
        o << findingText(fi);
        ++active;
    }
    if (!active)
        o << "No active findings.\n";

    section(o, QStringLiteral("CPU"));
    o << "busy " << fmt::percent(y.total.busy) << "  user " << fmt::percent(y.total.user) << "  sys "
      << fmt::percent(y.total.system) << "  irq " << fmt::percent(y.total.irq) << "  softirq "
      << fmt::percent(y.total.softirq) << "  iowait " << fmt::percent(y.total.iowait) << "\n";
    o << "load " << QString::number(y.load1, 'f', 2) << " " << QString::number(y.load5, 'f', 2) << " "
      << QString::number(y.load15, 'f', 2) << "  on " << y.onlineCpus << " CPUs;  running " << y.procsRunning
      << "  blocked(D) " << y.procsBlocked << "  threads " << y.threadsTotal << "\n";
    if (y.avgRunnable >= 0)
        o << "avg runnable " << QString::number(y.avgRunnable, 'f', 2) << "  avg blocked "
          << QString::number(y.avgBlocked, 'f', 2) << " (50 Hz)\n";
    o << "run-queue wait (avg tasks waiting) " << QString::number(y.runDelayTotalPct / 100.0, 'f', 2)
      << "  ctxsw " << fmt::rate(y.ctxtPs) << "  irq " << fmt::rate(y.intrPs) << "  forks " << fmt::rate(y.forksPs) << "\n";
    o << "cpufreq: " << y.policy.driver << " / " << y.policy.governor << "  EPP " << y.policy.epp << "  boost "
      << (y.policy.boost < 0 ? QStringLiteral("?") : QString::number(y.policy.boost)) << "  max "
      << fmt::mhz(y.policy.scalingMaxMHz) << " of " << fmt::mhz(y.policy.hwMaxMHz) << "\n";
    o << pad(QStringLiteral("cpu"), 5) << rpad(QStringLiteral("busy"), 7) << rpad(QStringLiteral("irq"), 7)
      << rpad(QStringLiteral("sirq"), 7) << rpad(QStringLiteral("rqwait"), 8) << rpad(QStringLiteral("freq"), 10)
      << rpad(QStringLiteral("irq/s"), 9) << rpad(QStringLiteral("sirq/s"), 9) << "\n";
    for (const CpuCoreSample& c : y.cpus) {
        if (!c.online)
            continue;
        o << pad(QString::number(c.cpu), 5) << rpad(fmt::percent(c.load.busy, 0), 7) << rpad(fmt::percent(c.load.irq, 0), 7)
          << rpad(fmt::percent(c.load.softirq, 0), 7) << rpad(fmt::percent(c.runDelayPct, 0), 8)
          << rpad(fmt::mhz(c.freqMHz), 10) << rpad(fmt::count(c.irqPs), 9) << rpad(fmt::count(c.softirqPs), 9) << "\n";
    }

    section(o, QStringLiteral("Memory"));
    o << "used " << fmt::kb(y.mem.usedKb) << " / " << fmt::kb(y.mem.totalKb) << "  available " << fmt::kb(y.mem.availableKb)
      << "  swap " << fmt::kb(y.mem.swapUsedKb) << " / " << fmt::kb(y.mem.swapTotalKb) << "  dirty " << fmt::kb(y.mem.dirtyKb)
      << "\n";
    o << "majflt/s " << fmt::count(y.vm.pgmajfault) << "  swapin/s " << fmt::count(y.vm.pswpin) << "  swapout/s "
      << fmt::count(y.vm.pswpout) << "  direct-scan/s " << fmt::count(y.vm.pgscanDirect) << "  allocstall/s "
      << fmt::count(y.vm.allocstall) << "  compact-stall/s " << fmt::count(y.vm.compactStall) << "\n";
    if (y.psi.available)
        o << "PSI avg10: cpu " << y.psi.cpuSome.avg10 << "  mem some " << y.psi.memSome.avg10 << " full "
          << y.psi.memFull.avg10 << "  io some " << y.psi.ioSome.avg10 << " full " << y.psi.ioFull.avg10 << "\n";
    else
        o << "PSI unavailable (boot with psi=1 to enable pressure stall information)\n";

    if (f.thermal.hasCpuTemp() || f.power.available || !f.gpus.empty()) {
        section(o, QStringLiteral("Thermals & power"));
        if (f.thermal.hasCpuTemp())
            o << "CPU " << f.thermal.cpuTempLabel << " " << fmt::celsius(f.thermal.cpuTempC) << " (limit "
              << fmt::celsius(f.thermal.tjmaxC) << ")\n";
        if (f.power.available)
            o << "package " << fmt::watts(f.power.packageW) << "  cores " << fmt::watts(f.power.coreW) << "  idle floor "
              << fmt::watts(f.power.idleFloorW) << "  attributable " << fmt::watts(f.power.attributableW) << "\n";
        for (const GpuSample& g : f.gpus) {
            o << "GPU" << g.index << " " << g.name << ": " << fmt::celsius(g.tempC) << " (slowdown "
              << fmt::celsius(g.slowdownC) << ")  " << fmt::watts(g.powerW) << " / " << fmt::watts(g.powerLimitW) << "  util "
              << g.utilGpu << "%  SM " << g.clockSmMHz << " MHz";
            if (g.reasonsValid)
                o << "  reasons: " << decodeGpuEventReasons(g.eventReasons).join(QStringLiteral(", "));
            o << "\n";
        }
        for (const SensorReading& r : f.sensors) {
            if (r.kind != SensorKind::Temp && r.kind != SensorKind::Fan)
                continue;
            o << "  " << pad(r.chip + QStringLiteral(" ") + r.label, 32)
              << (r.kind == SensorKind::Temp ? fmt::celsius(r.value) : QStringLiteral("%1 RPM").arg(r.value, 0, 'f', 0))
              << (r.slowChip ? "  (slow chip)" : "") << "\n";
        }
    }

    if (!f.procs.empty()) {
        section(o, QStringLiteral("Top processes by CPU"));
        std::vector<const ProcSample*> v;
        for (const ProcSample& p : f.procs)
            v.push_back(&p);
        std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->cpuPct > b->cpuPct; });
        o << rpad(QStringLiteral("pid"), 8) << "  " << pad(QStringLiteral("name"), 20) << rpad(QStringLiteral("cpu"), 8)
          << rpad(QStringLiteral("rqwait"), 10) << rpad(QStringLiteral("inv/s"), 8) << rpad(QStringLiteral("majf/s"), 8)
          << rpad(QStringLiteral("rss"), 11) << rpad(QStringLiteral("estW"), 7) << rpad(QStringLiteral("gpu"), 6) << "  unit\n";
        for (size_t i = 0; i < v.size() && i < 15; ++i) {
            const ProcSample& p = *v[i];
            o << rpad(QString::number(p.key.pid), 8) << "  " << pad(p.comm, 20) << rpad(fmt::percent(p.cpuPct), 8)
              << rpad(p.runDelayMsPs >= 0 ? fmt::ms(p.runDelayMsPs) : QStringLiteral("–"), 10)
              << rpad(p.nivcswPs >= 0 ? fmt::count(p.nivcswPs) : QStringLiteral("–"), 8) << rpad(fmt::count(p.majfltPs), 8)
              << rpad(fmt::bytes(double(p.rssBytes)), 11)
              << rpad(p.estCpuW >= 0 ? QString::number(p.estCpuW + std::max(0.0, p.estGpuW), 'f', 1) : QStringLiteral("–"), 7)
              << rpad(p.gpuPct >= 0 ? fmt::percent(p.gpuPct, 0) : QStringLiteral("–"), 6) << "  " << p.unit << "\n";
        }
    }

    if (!f.dstate.empty()) {
        section(o, QStringLiteral("Threads in uninterruptible sleep (D)"));
        for (const DStateThread& d : f.dstate)
            o << "  " << d.procComm << " (" << d.pid << ") thread " << d.comm << " (" << d.tid << ")  wchan "
              << (d.wchan.isEmpty() ? QStringLiteral("?") : d.wchan) << "  for " << fmt::duration(d.ageSec) << "\n";
    }

    if (!y.disks.empty()) {
        section(o, QStringLiteral("Disks"));
        for (const DiskSample& d : y.disks)
            o << "  " << pad(d.label, 28) << " util " << rpad(fmt::percent(d.utilPct, 0), 5) << "  r "
              << rpad(fmt::bytesPerSec(d.readBps), 10) << "  w " << rpad(fmt::bytesPerSec(d.writeBps), 10) << "  await "
              << fmt::ms(d.awaitMs) << "\n";
    }

    if (!y.irqs.empty()) {
        section(o, QStringLiteral("Busiest interrupts"));
        for (size_t i = 0; i < y.irqs.size() && i < 8; ++i) {
            const IrqRate& r = y.irqs[i];
            o << "  " << pad(r.label, 6) << pad(r.name, 30) << rpad(fmt::rate(r.perSec), 10) << "  top CPU " << r.topCpu
              << " (" << fmt::rate(r.topCpuPerSec) << ")\n";
        }
        for (const SoftirqRate& r : y.softirqs)
            if (r.perSec > 0)
                o << "  softirq " << pad(r.name, 9) << rpad(fmt::rate(r.perSec), 10) << "  top CPU " << r.topCpu << "\n";
    }

    section(o, QStringLiteral("Stutter detection"));
    if (f.stutter.probes > 0)
        o << f.stutter.probes << " probe(s), threshold " << fmt::ms(f.stutter.thresholdMs) << ", worst wakeup delay last interval "
          << fmt::ms(f.stutter.worstMs) << ", " << f.stutter.hitchesTotal << " hitch(es) so far"
          << (f.stutter.suppressed ? QStringLiteral(" (%1 not analysed: rate limit)").arg(f.stutter.suppressed) : QString()) << "\n";
    else
        o << "Latency probes are off.\n";
    if (!f.stutter.error.isEmpty())
        o << f.stutter.error << "\n";
    if (!hitches.empty()) {
        o << "\n";
        const size_t from = hitches.size() > 12 ? hitches.size() - 12 : 0;
        for (size_t i = from; i < hitches.size(); ++i)
            o << hitchText(hitches[i]);
        if (from)
            o << "(" << from << " earlier hitch(es) omitted)\n";
    }

    o << "\nCulprit itself: " << fmt::percent(f.self.cpuPct) << " CPU, " << fmt::bytes(double(f.self.rssBytes))
      << " RSS, frame built in " << fmt::ms(f.self.collectMs) << "\n";
    return s;
}

} // namespace culprit
