// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/tabs/OverviewTab.h"

#include "core/Format.h"
#include "gui/widgets/Sparkline.h"

#include <QGridLayout>
#include <QSplitter>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

namespace culprit {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}

OverviewTab::OverviewTab(QWidget* parent) : TabPage(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(6, 6, 6, 6);
    splitter_ = new QSplitter(Qt::Vertical, this);
    outer->addWidget(splitter_);

    auto* cards = new QWidget(splitter_);
    grid_ = new QGridLayout(cards);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(8);
    splitter_->addWidget(cards);
    cards->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    cpu_ = addCard(tr("CPU"), QStringLiteral("processes"));
    cpu_->setFixedRange(0, 100);
    cpu_->setSeries({tr("busy"), tr("irq+softirq")});

    load_ = addCard(tr("Load average"), QStringLiteral("load"));
    load_->setAutoRange(1);

    runq_ = addCard(tr("Run-queue wait"), QStringLiteral("load"));
    runq_->setAutoRange(0.5);
    runq_->setToolTip(tr("Average number of runnable tasks waiting for a CPU. Click to open Load & I/O."));

    mem_ = addCard(tr("Memory"), QStringLiteral("processes"));
    mem_->setFixedRange(0, 100);
    mem_->setSeries({tr("used"), tr("swap")});

    cpuTemp_ = addCard(tr("CPU temperature"), QStringLiteral("thermals"));
    cpuTemp_->setAutoRange(90);

    power_ = addCard(tr("CPU package power"), QStringLiteral("thermals"));
    power_->setAutoRange(50);

    gpu_ = addCard(tr("GPU"), QStringLiteral("thermals"));
    gpu_->setFixedRange(0, 100);
    gpu_->setSeries({tr("util"), tr("power %")});

    gpuTemp_ = addCard(tr("GPU temperature"), QStringLiteral("thermals"));
    gpuTemp_->setAutoRange(90);

    disk_ = addCard(tr("Disk I/O"), QStringLiteral("load"));
    disk_->setSeries({tr("read"), tr("write")});
    disk_->setAutoRange(1e6);

    net_ = addCard(tr("Network"), QStringLiteral("load"));
    net_->setSeries({tr("rx"), tr("tx")});
    net_->setAutoRange(1e5);

    faults_ = addCard(tr("Memory pressure"), QStringLiteral("load"));
    faults_->setSeries({tr("major faults/s"), tr("direct reclaim/s")});
    faults_->setAutoRange(10);

    latency_ = addCard(tr("Worst wakeup latency"), QStringLiteral("stutter"));
    latency_->setAutoRange(4);
    latency_->setValueText(QStringLiteral("–"));
}

Sparkline* OverviewTab::addCard(const QString& title, const QString& target)
{
    auto* s = new Sparkline(title);
    s->setCursor(Qt::PointingHandCursor);
    s->setToolTip(tr("Click to open the related tab"));
    connect(s, &Sparkline::clicked, this, [this, target] { emit navigate(target); });
    const int cols = 4;
    grid_->addWidget(s, cardCount_ / cols, cardCount_ % cols);
    ++cardCount_;
    return s;
}

void OverviewTab::setBottomWidget(QWidget* w)
{
    splitter_->addWidget(w);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
}

void OverviewTab::accumulate(const Frame& f)
{
    const SystemSample& s = f.sys;

    // CPU
    cpu_->push({s.total.busy, s.total.irq + s.total.softirq});
    cpu_->setValueText(fmt::percent(s.total.busy), s.total.busy > 90);
    cpu_->setSubText(tr("user %1 · sys %2 · irq %3 · iowait %4")
                         .arg(fmt::percent(s.total.user, 0), fmt::percent(s.total.system, 0),
                              fmt::percent(s.total.irq + s.total.softirq, 0), fmt::percent(s.total.iowait, 0)));

    // Load
    load_->setThreshold(s.onlineCpus);
    load_->push(s.load1);
    load_->setValueText(QString::number(s.load1, 'f', 2), s.load1 > s.onlineCpus);
    load_->setSubText(tr("5m %1 · 15m %2 · %3 CPUs · R %4 D %5")
                          .arg(s.load5, 0, 'f', 2)
                          .arg(s.load15, 0, 'f', 2)
                          .arg(s.onlineCpus)
                          .arg(s.procsRunning)
                          .arg(s.procsBlocked));

    // Run-queue wait: average number of runnable tasks waiting for a CPU.
    const double waiting = s.runDelayTotalPct / 100.0;
    runq_->push(waiting);
    runq_->setValueText(tr("%1 tasks").arg(waiting, 0, 'f', 2), waiting > 1.0);


    // Memory
    const double memPct = s.mem.totalKb ? double(s.mem.usedKb) * 100.0 / double(s.mem.totalKb) : 0;
    const double swapPct = s.mem.swapTotalKb ? double(s.mem.swapUsedKb) * 100.0 / double(s.mem.swapTotalKb) : kNaN;
    mem_->push({memPct, swapPct});
    mem_->setValueText(fmt::percent(memPct), memPct > 90);
    mem_->setSubText(tr("%1 of %2 · swap %3")
                         .arg(fmt::kb(s.mem.usedKb), fmt::kb(s.mem.totalKb), fmt::kb(s.mem.swapUsedKb)));

    // Thermals
    if (f.thermal.hasCpuTemp()) {
        cpuTemp_->setThreshold(f.thermal.tjmaxC);
        cpuTemp_->push(f.thermal.cpuTempC);
        cpuTemp_->setValueText(fmt::celsius(f.thermal.cpuTempC), f.thermal.cpuTempC >= f.thermal.tjmaxC - 5);
        cpuTemp_->setSubText(tr("%1 · limit %2").arg(f.thermal.cpuTempLabel, fmt::celsius(f.thermal.tjmaxC)));
    } else {
        cpuTemp_->push(kNaN);
        cpuTemp_->setValueText(QStringLiteral("–"));
        cpuTemp_->setSubText(tr("no CPU temperature sensor"));
    }

    if (f.power.available && f.power.packageW >= 0) {
        power_->push(f.power.packageW);
        power_->setValueText(fmt::watts(f.power.packageW));
        power_->setSubText(f.power.coreW >= 0 ? tr("cores %1 · uncore %2").arg(fmt::watts(f.power.coreW), fmt::watts(f.power.uncoreW))
                                              : tr("idle floor %1 · load-driven %2").arg(fmt::watts(f.power.idleFloorW), fmt::watts(f.power.attributableW)));
    } else {
        power_->push(kNaN);
        power_->setValueText(QStringLiteral("–"));
        power_->setSubText(tr("RAPL not readable"));
    }

    if (!f.gpus.empty()) {
        const GpuSample& g = f.gpus.front();
        const double powerPct = (g.powerW >= 0 && g.powerLimitW > 0) ? g.powerW * 100.0 / g.powerLimitW : kNaN;
        gpu_->push({double(g.utilGpu), powerPct});
        gpu_->setValueText(g.utilGpu >= 0 ? fmt::percent(g.utilGpu, 0) : QStringLiteral("–"));
        gpu_->setSubText(tr("%1 · %2").arg(g.name, fmt::watts(g.powerW)));
        gpuTemp_->setThreshold(g.slowdownC > 0 ? g.slowdownC : kNaN);
        gpuTemp_->push(g.tempC);
        const bool throttled = g.reasonsValid && (g.eventReasons & (GpuReason::SwThermal | GpuReason::HwThermal | GpuReason::HwSlowdown));
        gpuTemp_->setValueText(fmt::celsius(g.tempC), throttled);
        QStringList reasons = decodeGpuEventReasons(g.eventReasons & ~GpuReason::Idle);
        gpuTemp_->setSubText(reasons.isEmpty() ? tr("no throttling") : reasons.join(QStringLiteral(", ")));
    } else {
        gpu_->push(kNaN);
        gpu_->setValueText(QStringLiteral("–"));
        gpu_->setSubText(tr("no supported GPU"));
        gpuTemp_->push(kNaN);
        gpuTemp_->setValueText(QStringLiteral("–"));
    }

    // Disk / net
    double rd = 0, wr = 0, maxUtil = 0;
    QString busiest;
    for (const DiskSample& d : s.disks) {
        rd += d.readBps;
        wr += d.writeBps;
        if (d.utilPct > maxUtil) {
            maxUtil = d.utilPct;
            busiest = d.label;
        }
    }
    disk_->push({rd, wr});
    disk_->setValueText(fmt::bytesPerSec(rd + wr), maxUtil > 90);
    disk_->setSubText(busiest.isEmpty() ? tr("idle") : tr("busiest %1 at %2 util").arg(busiest, fmt::percent(maxUtil, 0)));

    double rx = 0, tx = 0;
    for (const NetSample& n : s.nets) {
        if (n.name == QLatin1String("lo"))
            continue;
        rx += n.rxBps;
        tx += n.txBps;
    }
    net_->push({rx, tx});
    net_->setValueText(fmt::bytesPerSec(rx + tx));
    net_->setSubText(tr("rx %1 · tx %2").arg(fmt::bytesPerSec(rx), fmt::bytesPerSec(tx)));

    const StutterSummary& st = f.stutter;
    if (st.probes > 0) {
        latency_->setThreshold(st.thresholdMs);
        latency_->push(std::max(0.0, st.worstMs));
        latency_->setValueText(fmt::ms(st.worstMs), st.worstMs >= st.thresholdMs);
        latency_->setSubText(tr("%1 probes · %2 hitches total").arg(st.probes).arg(st.hitchesTotal));
    } else {
        latency_->push(kNaN);
        latency_->setValueText(QStringLiteral("–"));
        latency_->setSubText(st.error.isEmpty() ? tr("latency probes off") : st.error);
    }

    faults_->push({s.vm.pgmajfault, s.vm.pgscanDirect});
    const bool pressure = s.vm.pgscanDirect > 0 || s.vm.pswpin > 100;
    faults_->setValueText(tr("%1 maj/s").arg(fmt::count(s.vm.pgmajfault)), pressure);
    faults_->setSubText(tr("swap in %1/s · out %2/s · avail %3")
                            .arg(fmt::count(s.vm.pswpin), fmt::count(s.vm.pswpout), fmt::kb(s.mem.availableKb)));
}

} // namespace culprit
