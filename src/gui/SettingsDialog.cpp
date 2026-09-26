// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/SettingsDialog.h"

#include "gui/ThemeManager.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace culprit {

namespace {

QDoubleSpinBox* dspin(double min, double max, double step, int decimals, const QString& suffix)
{
    auto* s = new QDoubleSpinBox;
    s->setRange(min, max);
    s->setSingleStep(step);
    s->setDecimals(decimals);
    s->setSuffix(suffix);
    return s;
}

QSpinBox* ispin(int min, int max, const QString& suffix)
{
    auto* s = new QSpinBox;
    s->setRange(min, max);
    s->setSuffix(suffix);
    return s;
}

} // namespace

SettingsDialog::SettingsDialog(const Settings& s, double detectedTjmax, QWidget* parent)
    : ChromeDialog(parent, tr("Culprit settings")), base_(s)
{
    QVBoxLayout* v = body();

    auto* appearance = new QGroupBox(tr("Appearance"));
    auto* f0 = new QFormLayout(appearance);
    theme_ = new QComboBox;
    theme_->addItem(ThemeManager::instance().systemPrefersDark() ? tr("Automatic (dark)") : tr("Automatic (light)"), int(ThemeMode::Auto));
    theme_->setToolTip(tr("Automatic follows the desktop's dark-mode setting; detected %1.").arg(ThemeManager::instance().systemDescription()));
    theme_->addItem(tr("Light"), int(ThemeMode::Light));
    theme_->addItem(tr("Dark"), int(ThemeMode::Dark));
    titleBar_ = new QComboBox;
    const TitleBarDecision autoTb = decideTitleBar(TitleBarMode::Auto);
    titleBar_->addItem(autoTb.integrated ? tr("Automatic (Culprit's own)") : tr("Automatic (system)"), int(TitleBarMode::Auto));
    titleBar_->addItem(tr("Culprit's own"), int(TitleBarMode::Integrated));
    titleBar_->addItem(tr("System"), int(TitleBarMode::System));
    titleBar_->setToolTip(tr("Automatic uses %1.").arg(autoTb.reason));
    f0->addRow(tr("Theme"), theme_);
    f0->addRow(tr("Title bar"), titleBar_);
    v->addWidget(appearance);

    auto* sampling = new QGroupBox(tr("Sampling"));
    auto* f1 = new QFormLayout(sampling);
    interval_ = ispin(250, 10000, tr(" ms"));
    interval_->setSingleStep(250);
    hotCount_ = ispin(5, 200, QString());
    hotCount_->setToolTip(tr("The busiest processes get a per-thread scan each interval (exact run-queue wait, "
                             "preemptions). More = more overhead."));
    f1->addRow(tr("Update interval"), interval_);
    f1->addRow(tr("Processes scanned per thread"), hotCount_);
    v->addWidget(sampling);

    auto* stutter = new QGroupBox(tr("Stutter detection"));
    auto* f2 = new QFormLayout(stutter);
    probeMode_ = new QComboBox;
    probeMode_->addItem(tr("Off"), int(ProbeMode::Off));
    probeMode_->addItem(tr("Floating probes"), int(ProbeMode::Floating));
    probeMode_->addItem(tr("Per-CPU sweep"), int(ProbeMode::PerCpu));
    probeMode_->addItem(tr("Kernel latency (RT)"), int(ProbeMode::Realtime));
    probes_ = ispin(1, 16, QString());
    floatThr_ = dspin(0.2, 100, 0.5, 1, tr(" ms"));
    cpuThr_ = dspin(0.2, 100, 0.5, 1, tr(" ms"));
    rtThr_ = dspin(0.05, 50, 0.1, 2, tr(" ms"));
    cpuThr_->setToolTip(tr("Keep this above the scheduler's base slice (~3 ms): waiting one slice behind a busy "
                           "task on the same CPU is normal fair scheduling."));
    f2->addRow(tr("Probe mode"), probeMode_);
    f2->addRow(tr("Floating probes"), probes_);
    f2->addRow(tr("Hitch threshold (floating)"), floatThr_);
    f2->addRow(tr("Hitch threshold (per-CPU)"), cpuThr_);
    f2->addRow(tr("Hitch threshold (kernel latency)"), rtThr_);
    v->addWidget(stutter);

    auto* rules = new QGroupBox(tr("Diagnosis"));
    auto* f3 = new QFormLayout(rules);
    tjmax_ = dspin(0, 125, 1, 0, tr(" °C"));
    tjmax_->setSpecialValueText(tr("auto (%1 °C)").arg(detectedTjmax, 0, 'f', 0));
    highCpu_ = dspin(0.1, 64, 0.1, 1, tr(" cores"));
    sustain_ = ispin(1, 600, tr(" s"));
    tempWarn_ = dspin(40, 110, 1, 0, tr(" °C"));
    raise_ = ispin(1, 30, tr(" evaluations"));
    clear_ = ispin(1, 120, tr(" evaluations"));
    f3->addRow(tr("CPU temperature limit (Tjmax)"), tjmax_);
    f3->addRow(tr("Report processes using ≥"), highCpu_);
    f3->addRow(tr("… sustained for"), sustain_);
    f3->addRow(tr("Warn when CPU is above"), tempWarn_);
    f3->addRow(tr("Show a finding after"), raise_);
    f3->addRow(tr("Clear a finding after"), clear_);
    v->addWidget(rules);

    auto* rec = new QGroupBox(tr("Recordings"));
    auto* f4 = new QHBoxLayout(rec);
    recDir_ = new QLineEdit;
    recDir_->setPlaceholderText(Settings().effectiveRecordingsDir());
    auto* browse = new QPushButton(tr("Browse…"));
    f4->addWidget(recDir_, 1);
    f4->addWidget(browse);
    v->addWidget(rec);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Recordings directory"),
                                                            recDir_->text().isEmpty() ? recDir_->placeholderText() : recDir_->text());
        if (!d.isEmpty())
            recDir_->setText(d);
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults);
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("primary"));
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this] { load(Settings()); });

    load(s);
}

void SettingsDialog::load(const Settings& s)
{
    theme_->setCurrentIndex(theme_->findData(int(s.themeMode)));
    titleBar_->setCurrentIndex(titleBar_->findData(int(s.titleBarMode)));
    interval_->setValue(s.intervalMs);
    hotCount_->setValue(s.hotProcessCount);
    probeMode_->setCurrentIndex(probeMode_->findData(int(s.probeMode)));
    probes_->setValue(s.floatingProbes);
    floatThr_->setValue(s.floatingThresholdMs);
    cpuThr_->setValue(s.perCpuThresholdMs);
    rtThr_->setValue(s.realtimeThresholdMs);
    tjmax_->setValue(s.tjmaxC);
    highCpu_->setValue(s.highCpuCores);
    sustain_->setValue(s.highCpuSustainSec);
    tempWarn_->setValue(s.cpuTempWarnC);
    raise_->setValue(s.findingRaiseAfter);
    clear_->setValue(s.findingClearAfter);
    recDir_->setText(s.recordingsDir);
}

Settings SettingsDialog::settings() const
{
    Settings s = base_;
    s.themeMode = ThemeMode(theme_->currentData().toInt());
    s.titleBarMode = TitleBarMode(titleBar_->currentData().toInt());
    s.intervalMs = interval_->value();
    s.hotProcessCount = hotCount_->value();
    s.probeMode = ProbeMode(probeMode_->currentData().toInt());
    s.floatingProbes = probes_->value();
    s.floatingThresholdMs = floatThr_->value();
    s.perCpuThresholdMs = cpuThr_->value();
    s.realtimeThresholdMs = rtThr_->value();
    s.tjmaxC = tjmax_->value();
    s.highCpuCores = highCpu_->value();
    s.highCpuSustainSec = sustain_->value();
    s.cpuTempWarnC = tempWarn_->value();
    s.findingRaiseAfter = raise_->value();
    s.findingClearAfter = clear_->value();
    s.recordingsDir = recDir_->text().trimmed();
    return s;
}

} // namespace culprit
