// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/tabs/ThermalsTab.h"

#include "core/Format.h"
#include "gui/Theme.h"
#include "gui/widgets/CpuHeatmap.h"
#include "gui/widgets/Sparkline.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QScrollArea>
#include <QSplitter>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace culprit {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

QString sensorValue(SensorKind k, double v)
{
    switch (k) {
    case SensorKind::Temp: return fmt::celsius(v);
    case SensorKind::Fan: return QStringLiteral("%1 RPM").arg(v, 0, 'f', 0);
    case SensorKind::Voltage: return QStringLiteral("%1 V").arg(v, 0, 'f', 3);
    case SensorKind::Power: return fmt::watts(v);
    case SensorKind::Current: return QStringLiteral("%1 A").arg(v, 0, 'f', 2);
    case SensorKind::Other: return QString::number(v);
    }
    return {};
}
} // namespace

ThermalsTab::ThermalsTab(QWidget* parent) : TabPage(parent)
{
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(6, 6, 6, 6);
    auto* split = new QSplitter(Qt::Horizontal);
    outer->addWidget(split);

    // Left: all sensors
    sensors_ = new QTreeWidget;
    sensors_->setColumnCount(5);
    sensors_->setHeaderLabels({tr("Sensor"), tr("Value"), tr("Min"), tr("Max"), tr("Limit")});
    sensors_->setUniformRowHeights(true);
    sensors_->setAlternatingRowColors(true);
    sensors_->setColumnWidth(0, 210);
    for (int c = 1; c < 5; ++c)
        sensors_->setColumnWidth(c, 78);
    sensors_->setMinimumWidth(360);
    split->addWidget(sensors_);

    // Right: heatmap, graphs, GPU, contributors
    auto* right = new QWidget;
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);

    auto* cpuBox = new QGroupBox(tr("CPU cores"));
    auto* cbl = new QVBoxLayout(cpuBox);
    heatmap_ = new CpuHeatmap;
    policy_ = new QLabel;
    policy_->setWordWrap(true);
    cbl->addWidget(heatmap_);
    cbl->addWidget(policy_);
    rv->addWidget(cpuBox);

    auto* graphs = new QGridLayout;
    cpuTemp_ = new Sparkline(tr("CPU temperature"));
    cpuTemp_->setAutoRange(90);
    cpuPower_ = new Sparkline(tr("CPU power"));
    cpuPower_->setSeries({tr("package"), tr("load-driven")});
    cpuPower_->setAutoRange(50);
    gpuTemp_ = new Sparkline(tr("GPU temperature"));
    gpuTemp_->setAutoRange(90);
    gpuPower_ = new Sparkline(tr("GPU power"));
    gpuPower_->setAutoRange(100);
    for (Sparkline* s : {cpuTemp_, cpuPower_, gpuTemp_, gpuPower_})
        s->setCapacity(300);
    graphs->addWidget(cpuTemp_, 0, 0);
    graphs->addWidget(cpuPower_, 0, 1);
    graphs->addWidget(gpuTemp_, 1, 0);
    graphs->addWidget(gpuPower_, 1, 1);
    rv->addLayout(graphs);

    gpuInfo_ = new QLabel;
    gpuInfo_->setWordWrap(true);
    gpuInfo_->setTextFormat(Qt::RichText);
    rv->addWidget(gpuInfo_);

    auto* contribBox = new QGroupBox(tr("Heat contributors"));
    auto* cl = new QVBoxLayout(contribBox);
    contributors_ = new QTableWidget(0, 7);
    contributors_->setHorizontalHeaderLabels(
        {tr("Process"), tr("PID"), tr("Est. total W"), tr("CPU W"), tr("GPU W"), tr("CPU %"), tr("GPU %")});
    contributors_->verticalHeader()->hide();
    contributors_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    contributors_->setSelectionBehavior(QAbstractItemView::SelectRows);
    contributors_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    contributors_->setToolTip(tr("CPU core power (RAPL) split by CPU time × clock frequency; GPU power above its idle "
                                 "floor split by per-process GPU utilisation. Estimates, not measurements."));
    cl->addWidget(contributors_);
    rv->addWidget(contribBox, 1);
    connect(contributors_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (auto* it = contributors_->item(row, 1))
            emit processActivated(it->text().toInt());
    });

    split->addWidget(right);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({560, 1000});
}

void ThermalsTab::accumulate(const Frame& f)
{
    cpuTemp_->setThreshold(f.thermal.tjmaxC);
    if (f.thermal.hasCpuTemp()) {
        cpuTemp_->push(f.thermal.cpuTempC);
        cpuTemp_->setValueText(fmt::celsius(f.thermal.cpuTempC), f.thermal.cpuTempC >= f.thermal.tjmaxC - 5);
        cpuTemp_->setSubText(tr("%1 · Tjmax %2").arg(f.thermal.cpuTempLabel, fmt::celsius(f.thermal.tjmaxC)));
    } else {
        cpuTemp_->push(kNaN);
    }
    if (f.power.available) {
        cpuPower_->push({f.power.packageW, f.power.attributableW});
        cpuPower_->setValueText(fmt::watts(f.power.packageW));
        cpuPower_->setSubText(f.power.coreW >= 0
                                  ? tr("cores %1 · uncore/SoC %2").arg(fmt::watts(f.power.coreW), fmt::watts(f.power.uncoreW))
                                  : tr("idle floor %1 · load-driven %2").arg(fmt::watts(f.power.idleFloorW), fmt::watts(f.power.attributableW)));
    } else {
        cpuPower_->push(kNaN);
        cpuPower_->setValueText(QStringLiteral("–"));
        cpuPower_->setSubText(tr("RAPL not readable"));
    }
    if (!f.gpus.empty()) {
        const GpuSample& g = f.gpus.front();
        gpuTemp_->setThreshold(g.slowdownC > 0 ? g.slowdownC : kNaN);
        gpuTemp_->push(g.tempC);
        gpuTemp_->setValueText(fmt::celsius(g.tempC), g.slowdownC > 0 && g.tempC >= g.slowdownC - 5);
        gpuTemp_->setSubText(tr("slowdown at %1").arg(fmt::celsius(g.slowdownC)));
        gpuPower_->setThreshold(g.powerLimitW > 0 ? g.powerLimitW : kNaN);
        gpuPower_->push(g.powerW);
        gpuPower_->setValueText(fmt::watts(g.powerW));
        gpuPower_->setSubText(tr("limit %1").arg(fmt::watts(g.powerLimitW)));
    } else {
        gpuTemp_->push(kNaN);
        gpuPower_->push(kNaN);
    }

    // Flag temperature channels that are almost certainly not wired to a sensor.
    // Sticky for the session so the mark doesn't flicker as the CPU warms up.
    for (const SensorReading& r : f.sensors) {
        if (r.kind != SensorKind::Temp || notConnected_.contains(r.key))
            continue;
        if (r.value <= 0) {
            notConnected_.insert(r.key, tr("Probably not connected: reads %1. The board doesn't report this channel, or nothing "
                                           "is attached to it.").arg(fmt::celsius(r.value)));
        } else if (f.thermal.hasCpuTemp() && r.value >= 90 && r.value >= f.thermal.cpuTempC + 10) {
            notConnected_.insert(r.key, tr("Probably not connected: reads %1 while the CPU is at %2 — a board sensor can't run "
                                           "that much hotter than the CPU. Unused thermistor inputs on the monitoring chip float "
                                           "to a near-maximum value (for example an empty T_SENSOR header).")
                                            .arg(fmt::celsius(r.value), fmt::celsius(f.thermal.cpuTempC)));
        }
    }

    // Track session min/max per sensor even while hidden.
    for (const SensorReading& r : f.sensors) {
        auto it = minMax_.find(r.key);
        if (it == minMax_.end())
            minMax_.insert(r.key, {r.value, r.value});
        else
            it->first = std::min(it->first, r.value), it->second = std::max(it->second, r.value);
    }
}

void ThermalsTab::render(const Frame& f)
{
    heatmap_->setData(f.sys);
    const CpuPolicy& pol = f.sys.policy;
    QStringList parts{pol.driver, pol.governor};
    if (!pol.epp.isEmpty())
        parts << tr("EPP %1").arg(pol.epp);
    if (pol.boost >= 0)
        parts << (pol.boost ? tr("boost on") : tr("boost off"));
    parts << tr("max %1").arg(fmt::mhz(pol.scalingMaxMHz));
    policy_->setText(parts.join(QStringLiteral(" · ")));
    policy_->setToolTip(tr("Frequency driver · governor · energy/performance preference · boost · frequency cap "
                           "(hardware maximum %1)").arg(fmt::mhz(pol.hwMaxMHz)));
    updateSensors(f);
    updateGpu(f);
    updateContributors(f);
}

void ThermalsTab::updateSensors(const Frame& f)
{
    for (const SensorReading& r : f.sensors) {
        QTreeWidgetItem* chip = chipItems_.value(r.chip);
        if (!chip) {
            chip = new QTreeWidgetItem(sensors_, {r.chip});
            if (r.slowChip)
                chip->setToolTip(0, tr("Slow sensor chip: each read costs ~70 ms of kernel CPU, so it is read every 30 s "
                                       "(every 5 s while this tab is open)."));
            chip->setFirstColumnSpanned(true);
            chip->setExpanded(true);
            QFont bf = chip->font(0);
            bf.setBold(true);
            chip->setFont(0, bf);
            chipItems_.insert(r.chip, chip);
        }
        QTreeWidgetItem* item = sensorItems_.value(r.key);
        if (!item) {
            item = new QTreeWidgetItem(chip, {r.label});
            for (int c = 1; c < 5; ++c)
                item->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
            sensorItems_.insert(r.key, item);
        }
        const auto mm = minMax_.value(r.key, {r.value, r.value});
        item->setText(1, sensorValue(r.kind, r.value));
        item->setText(2, sensorValue(r.kind, mm.first));
        item->setText(3, sensorValue(r.kind, mm.second));
        const double limit = r.crit > 0 ? r.crit : r.max;
        item->setText(4, limit > 0 ? sensorValue(r.kind, limit) : QString());
        const QString why = notConnected_.value(r.key);
        item->setText(0, why.isEmpty() ? r.label : r.label + QStringLiteral(" *"));
        for (int c = 0; c < 5; ++c)
            item->setToolTip(c, why);
        const bool hot = why.isEmpty() && r.kind == SensorKind::Temp && limit > 0 && r.value >= limit - 5 && r.value < 120;
        item->setForeground(1, hot ? QBrush(theme::red()) : QBrush());
    }
}

void ThermalsTab::updateGpu(const Frame& f)
{
    if (f.gpus.empty()) {
        gpuInfo_->setText(tr("<i>No NVIDIA GPU</i>"));
        return;
    }
    QString html, tip;
    for (const GpuSample& g : f.gpus) {
        const QStringList reasons = decodeGpuEventReasons(g.eventReasons);
        const bool bad = g.eventReasons & (GpuReason::SwThermal | GpuReason::HwThermal | GpuReason::HwSlowdown | GpuReason::HwPowerBrake);
        html += tr("<b>%1</b> · %2 · %3 / %4 · %5% · %6 MHz · fan %7 · %8 / %9 VRAM · <span style='color:%10'>%11</span><br>")
                    .arg(g.name.toHtmlEscaped(), fmt::celsius(g.tempC), fmt::watts(g.powerW), fmt::watts(g.powerLimitW))
                    .arg(g.utilGpu)
                    .arg(g.clockSmMHz)
                    .arg(g.fanPct >= 0 ? QString::number(g.fanPct) + QLatin1Char('%') : tr("n/a"))
                    .arg(fmt::bytes(double(g.memUsed)), fmt::bytes(double(g.memTotal)),
                         bad ? theme::red().name() : palette().color(QPalette::Text).name(),
                         reasons.isEmpty() ? tr("not limited") : reasons.join(QStringLiteral(", ")));
        tip += tr("GPU %1: temperature · power / limit · utilisation · SM clock · fan · VRAM · clock limiters\n"
                  "slowdown at %2, max operating %3; SM max %4 MHz; memory clock %5 MHz; memory controller %6%\n")
                   .arg(g.index)
                   .arg(fmt::celsius(g.slowdownC), fmt::celsius(g.maxOperatingC))
                   .arg(g.clockSmMaxMHz)
                   .arg(g.clockMemMHz)
                   .arg(g.utilMem);
    }
    gpuInfo_->setText(html);
    gpuInfo_->setToolTip(tip.trimmed());
}

void ThermalsTab::updateContributors(const Frame& f)
{
    std::vector<const ProcSample*> v;
    for (const ProcSample& p : f.procs)
        if (std::max(0.0, p.estCpuW) + std::max(0.0, p.estGpuW) >= 0.05)
            v.push_back(&p);
    auto total = [](const ProcSample* p) { return std::max(0.0, p->estCpuW) + std::max(0.0, p->estGpuW); };
    std::sort(v.begin(), v.end(), [&](auto* a, auto* b) { return total(a) > total(b); });
    if (v.size() > 20)
        v.resize(20);
    contributors_->setRowCount(int(v.size()));
    auto num = [](double x, int dec) {
        auto* it = new QTableWidgetItem(x >= 0 ? QString::number(x, 'f', dec) : QStringLiteral("–"));
        it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return it;
    };
    for (int r = 0; r < int(v.size()); ++r) {
        const ProcSample& p = *v[size_t(r)];
        contributors_->setItem(r, 0, new QTableWidgetItem(p.comm));
        contributors_->setItem(r, 1, num(p.key.pid, 0));
        contributors_->setItem(r, 2, num(total(&p), 1));
        contributors_->setItem(r, 3, num(p.estCpuW, 1));
        contributors_->setItem(r, 4, num(p.estGpuW, 1));
        contributors_->setItem(r, 5, num(p.cpuPct, 1));
        contributors_->setItem(r, 6, num(p.gpuPct, 0));
    }
}

} // namespace culprit
