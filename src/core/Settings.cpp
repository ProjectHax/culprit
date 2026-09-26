// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/Settings.h"

#include <QSettings>
#include <QStandardPaths>

namespace culprit {

QString probeModeName(ProbeMode m)
{
    switch (m) {
    case ProbeMode::Off: return QStringLiteral("Off");
    case ProbeMode::Floating: return QStringLiteral("Floating probes");
    case ProbeMode::PerCpu: return QStringLiteral("Per-CPU sweep");
    case ProbeMode::Realtime: return QStringLiteral("Kernel latency (RT)");
    }
    return {};
}

Settings Settings::load()
{
    QSettings q(QStringLiteral("culprit"), QStringLiteral("culprit"));
    Settings s;
    s.themeMode = ThemeMode(qBound(0, q.value("appearance/theme", int(s.themeMode)).toInt(), 2));
    s.titleBarMode = TitleBarMode(qBound(0, q.value("appearance/titleBar", int(s.titleBarMode)).toInt(), 2));
    s.intervalMs = qBound(250, q.value("sampling/intervalMs", s.intervalMs).toInt(), 10000);
    s.hotProcessCount = qBound(5, q.value("sampling/hotProcessCount", s.hotProcessCount).toInt(), 200);
    s.probeMode = ProbeMode(qBound(0, q.value("stutter/probeMode", int(s.probeMode)).toInt(), 3));
    s.floatingProbes = qBound(1, q.value("stutter/floatingProbes", s.floatingProbes).toInt(), 16);
    s.floatingThresholdMs = q.value("stutter/floatingThresholdMs", s.floatingThresholdMs).toDouble();
    s.perCpuThresholdMs = q.value("stutter/perCpuThresholdMs", s.perCpuThresholdMs).toDouble();
    s.realtimeThresholdMs = q.value("stutter/realtimeThresholdMs", s.realtimeThresholdMs).toDouble();
    s.tjmaxC = q.value("thermal/tjmaxC", s.tjmaxC).toDouble();
    s.highCpuCores = q.value("rules/highCpuCores", s.highCpuCores).toDouble();
    s.highCpuSustainSec = q.value("rules/highCpuSustainSec", s.highCpuSustainSec).toInt();
    s.cpuTempWarnC = q.value("rules/cpuTempWarnC", s.cpuTempWarnC).toDouble();
    s.findingRaiseAfter = qBound(1, q.value("rules/raiseAfter", s.findingRaiseAfter).toInt(), 30);
    s.findingClearAfter = qBound(1, q.value("rules/clearAfter", s.findingClearAfter).toInt(), 120);
    s.recordingsDir = q.value("recorder/dir").toString();
    return s;
}

void Settings::save() const
{
    QSettings q(QStringLiteral("culprit"), QStringLiteral("culprit"));
    q.setValue("appearance/theme", int(themeMode));
    q.setValue("appearance/titleBar", int(titleBarMode));
    q.setValue("sampling/intervalMs", intervalMs);
    q.setValue("sampling/hotProcessCount", hotProcessCount);
    q.setValue("stutter/probeMode", int(probeMode));
    q.setValue("stutter/floatingProbes", floatingProbes);
    q.setValue("stutter/floatingThresholdMs", floatingThresholdMs);
    q.setValue("stutter/perCpuThresholdMs", perCpuThresholdMs);
    q.setValue("stutter/realtimeThresholdMs", realtimeThresholdMs);
    q.setValue("thermal/tjmaxC", tjmaxC);
    q.setValue("rules/highCpuCores", highCpuCores);
    q.setValue("rules/highCpuSustainSec", highCpuSustainSec);
    q.setValue("rules/cpuTempWarnC", cpuTempWarnC);
    q.setValue("rules/raiseAfter", findingRaiseAfter);
    q.setValue("rules/clearAfter", findingClearAfter);
    q.setValue("recorder/dir", recordingsDir);
}

QString Settings::effectiveRecordingsDir() const
{
    if (!recordingsDir.isEmpty())
        return recordingsDir;
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/culprit/recordings");
}

} // namespace culprit
