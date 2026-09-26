// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/tabs/LoadIoTab.h"

#include "core/Format.h"
#include "gui/Theme.h"
#include "gui/widgets/DataTable.h"
#include "gui/widgets/Sparkline.h"

#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QTabWidget>
#include <QVBoxLayout>

namespace culprit {

namespace {
using Cell = DataTable::Cell;

Cell pct(double v, double warnAt = 1e9)
{
    return Cell(v >= 0.05 ? fmt::percent(v) : QString(), v, v >= warnAt ? theme::red() : QColor());
}
Cell num(double v, const QString& text, double warnAt = 1e9) { return Cell(text, v, v >= warnAt ? theme::red() : QColor()); }
} // namespace

LoadIoTab::LoadIoTab(QWidget* parent) : TabPage(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);

    auto* top = new QGridLayout;
    summary_ = new QLabel;
    summary_->setWordWrap(true);
    summary_->setTextFormat(Qt::RichText);
    loadGraph_ = new Sparkline(tr("Load vs runnable / blocked"));
    loadGraph_->setSeries({tr("load 1m"), tr("runnable"), tr("blocked (D)")});
    loadGraph_->setAutoRange(2);
    loadGraph_->setCapacity(300);
    blockedGraph_ = new Sparkline(tr("Run-queue wait"));
    blockedGraph_->setSeries({tr("avg tasks waiting"), tr("iowait %")});
    blockedGraph_->setAutoRange(1);
    blockedGraph_->setCapacity(300);
    top->addWidget(summary_, 0, 0, 1, 2);
    top->addWidget(loadGraph_, 1, 0);
    top->addWidget(blockedGraph_, 1, 1);
    v->addLayout(top);

    sub_ = new QTabWidget;
    v->addWidget(sub_, 1);

    cpus_ = new DataTable({tr("CPU"), tr("Busy"), tr("User"), tr("System"), tr("IRQ"), tr("Softirq"), tr("IOwait"),
                           tr("RQ wait"), tr("Freq"), tr("IRQs/s"), tr("Softirqs/s")});
    cpus_->setNumericColumns({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
    cpus_->horizontalHeaderItem(7)->setToolTip(tr("Time tasks spent runnable but waiting for this CPU, as % of wall time."));
    sub_->addTab(cpus_, tr("Per CPU"));

    dstate_ = new DataTable({tr("Process"), tr("PID"), tr("Thread"), tr("TID"), tr("Blocked for"), tr("Wait channel"), tr("Kernel stack")});
    dstate_->setNumericColumns({1, 3, 4});
    dstate_->setEmptyText(tr("No blocked threads."));
    sub_->addTab(dstate_, tr("Blocked (D) threads"));
    connect(dstate_, &DataTable::cellDoubleClicked, this, [this](int r, int) {
        if (auto* it = dstate_->item(r, 1))
            emit processActivated(it->text().toInt());
    });

    disks_ = new DataTable({tr("Device"), tr("Util"), tr("Read/s"), tr("Write/s"), tr("Read IOPS"), tr("Write IOPS"),
                            tr("Avg wait"), tr("Read wait"), tr("Write wait"), tr("In flight")});
    disks_->setNumericColumns({1, 2, 3, 4, 5, 6, 7, 8, 9});
    sub_->addTab(disks_, tr("Disks"));

    irqs_ = new DataTable({tr("IRQ"), tr("Device / handler"), tr("Rate"), tr("Busiest CPU"), tr("Rate on that CPU")});
    irqs_->setNumericColumns({2, 3, 4});
    sub_->addTab(irqs_, tr("Interrupts"));

    softirqs_ = new DataTable({tr("Softirq"), tr("Rate"), tr("Busiest CPU"), tr("Rate on that CPU")});
    softirqs_->setNumericColumns({1, 2, 3});
    sub_->addTab(softirqs_, tr("Softirqs"));

    cgroups_ = new DataTable({tr("Unit / cgroup"), tr("CPU quota"), tr("Throttled periods"), tr("Throttled time"),
                              tr("memory.high events"), tr("OOM kills"), tr("Path")});
    cgroups_->setNumericColumns({1, 2, 3, 4, 5});
    cgroups_->setEmptyText(tr("No throttled cgroups."));
    sub_->addTab(cgroups_, tr("Cgroups"));

    nets_ = new DataTable({tr("Interface"), tr("Receive"), tr("Transmit"), tr("Packets in/s"), tr("Packets out/s"), tr("Errors/s"), tr("Drops/s")});
    nets_->setNumericColumns({1, 2, 3, 4, 5, 6});
    sub_->addTab(nets_, tr("Network"));

    dstate_->horizontalHeader()->setStretchLastSection(true);
}

void LoadIoTab::accumulate(const Frame& f)
{
    const SystemSample& s = f.sys;
    const double r = s.avgRunnable >= 0 ? s.avgRunnable : s.procsRunning;
    const double d = s.avgBlocked >= 0 ? s.avgBlocked : s.procsBlocked;
    loadGraph_->setThreshold(s.onlineCpus);
    loadGraph_->push({s.load1, r, d});
    loadGraph_->setValueText(QString::number(s.load1, 'f', 2), s.load1 > s.onlineCpus);
    blockedGraph_->push({s.runDelayTotalPct / 100.0, s.total.iowait});
    blockedGraph_->setValueText(tr("%1 waiting").arg(s.runDelayTotalPct / 100.0, 0, 'f', 2), s.runDelayTotalPct >= 100);
}

void LoadIoTab::render(const Frame& f)
{
    const SystemSample& s = f.sys;
    const double r = s.avgRunnable >= 0 ? s.avgRunnable : s.procsRunning;
    const double d = s.avgBlocked >= 0 ? s.avgBlocked : s.procsBlocked;
    QString text = tr("<b>Load</b> %1 · %2 · %3 · %4 CPUs · runnable %5 · blocked %6 · busy %7 · iowait %8 · %9 ctx/s · %10 irq/s")
                       .arg(s.load1, 0, 'f', 2)
                       .arg(s.load5, 0, 'f', 2)
                       .arg(s.load15, 0, 'f', 2)
                       .arg(s.onlineCpus)
                       .arg(r, 0, 'f', 1)
                       .arg(d, 0, 'f', 1)
                       .arg(fmt::percent(s.total.busy), fmt::percent(s.total.iowait), fmt::count(s.ctxtPs), fmt::count(s.intrPs));
    if (s.psi.available)
        text += tr(" · <b>PSI</b> cpu %1% · mem %2% · io %3%").arg(s.psi.cpuSome.avg10).arg(s.psi.memSome.avg10).arg(s.psi.ioSome.avg10);
    summary_->setText(text);
    summary_->setToolTip(tr("Load average (1, 5, 15 min). Runnable and blocked are averaged at 50 Hz; blocked means "
                            "uninterruptible sleep (D state), usually waiting on I/O.%1")
                             .arg(s.psi.available ? tr(" PSI: share of time tasks stalled (avg10).") : QString()));

    std::vector<DataTable::Row> rows;
    for (const CpuCoreSample& c : s.cpus) {
        if (!c.online)
            continue;
        rows.push_back({num(c.cpu, QString::number(c.cpu)), pct(c.load.busy, 95), pct(c.load.user), pct(c.load.system),
                        pct(c.load.irq, 20), pct(c.load.softirq, 20), pct(c.load.iowait), pct(c.runDelayPct, 50),
                        num(c.freqMHz, fmt::mhz(c.freqMHz)), num(c.irqPs, fmt::count(c.irqPs)), num(c.softirqPs, fmt::count(c.softirqPs))});
    }
    cpus_->setRows(rows);

    rows.clear();
    for (const DStateThread& t : f.dstate) {
        DataTable::Row row{Cell(t.procComm), num(t.pid, QString::number(t.pid)), Cell(t.comm), num(t.tid, QString::number(t.tid)),
                           num(t.ageSec, fmt::duration(t.ageSec), 10),
                           Cell(t.wchan.isEmpty() ? tr("(hidden — needs Deep trace)") : t.wchan), Cell(t.stack.join(QStringLiteral(" ← ")))};
        row[6].tooltip = t.stack.join(QLatin1Char('\n'));
        rows.push_back(row);
    }
    dstate_->setRows(rows);
    sub_->setTabText(1, f.dstate.empty() ? tr("Blocked (D) threads") : tr("Blocked (D) threads (%1)").arg(f.dstate.size()));

    rows.clear();
    for (const DiskSample& dk : s.disks) {
        DataTable::Row row{Cell(dk.label == dk.name ? dk.name : QStringLiteral("%1 (%2)").arg(dk.label, dk.name)),
                           pct(dk.utilPct, 90), num(dk.readBps, fmt::bytesPerSec(dk.readBps)), num(dk.writeBps, fmt::bytesPerSec(dk.writeBps)),
                           num(dk.readsPs, fmt::count(dk.readsPs)), num(dk.writesPs, fmt::count(dk.writesPs)),
                           num(dk.awaitMs, fmt::ms(dk.awaitMs), 50), num(dk.readAwaitMs, fmt::ms(dk.readAwaitMs), 50),
                           num(dk.writeAwaitMs, fmt::ms(dk.writeAwaitMs), 50), num(double(dk.inFlight), QString::number(dk.inFlight))};
        rows.push_back(row);
    }
    disks_->setRows(rows);

    rows.clear();
    for (const IrqRate& i : s.irqs)
        rows.push_back({Cell(i.label), Cell(i.name), num(i.perSec, fmt::rate(i.perSec)), num(i.topCpu, QString::number(i.topCpu)),
                        num(i.topCpuPerSec, fmt::rate(i.topCpuPerSec))});
    irqs_->setRows(rows);

    rows.clear();
    for (const SoftirqRate& i : s.softirqs)
        rows.push_back({Cell(i.name), num(i.perSec, fmt::rate(i.perSec)), num(i.topCpu, QString::number(i.topCpu)),
                        num(i.topCpuPerSec, fmt::rate(i.topCpuPerSec))});
    softirqs_->setRows(rows);

    rows.clear();
    for (const CgroupSample& c : f.cgroups)
        rows.push_back({Cell(c.unit),
                        num(c.cpuQuotaCores, c.cpuQuotaCores > 0 ? tr("%1 cores").arg(c.cpuQuotaCores, 0, 'f', 2) : tr("none")),
                        pct(c.throttledPct, 10), num(c.throttledMsPs, tr("%1/s").arg(fmt::ms(c.throttledMsPs)), 20),
                        num(double(c.memHighEvents), QString::number(c.memHighEvents), 1), num(double(c.oomKills), QString::number(c.oomKills), 1),
                        Cell(c.path)});
    cgroups_->setRows(rows);

    rows.clear();
    for (const NetSample& n : s.nets)
        rows.push_back({Cell(n.name), num(n.rxBps, fmt::bytesPerSec(n.rxBps)), num(n.txBps, fmt::bytesPerSec(n.txBps)),
                        num(n.rxPps, fmt::count(n.rxPps)), num(n.txPps, fmt::count(n.txPps)), num(n.errsPs, fmt::count(n.errsPs), 1),
                        num(n.dropsPs, fmt::count(n.dropsPs), 1)});
    nets_->setRows(rows);
}

} // namespace culprit
