// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include <QString>

namespace culprit {

enum class ProbeMode { Off = 0, Floating = 1, PerCpu = 2, Realtime = 3 };
enum class ThemeMode { Auto = 0, Light = 1, Dark = 2 };
enum class TitleBarMode { Auto = 0, Integrated = 1, System = 2 };

QString probeModeName(ProbeMode m);

struct Settings {
    // Appearance
    ThemeMode themeMode = ThemeMode::Auto;
    TitleBarMode titleBarMode = TitleBarMode::Auto;

    // Sampling
    int intervalMs = 1000;           // engine frame interval
    int hotProcessCount = 40;        // processes that get a per-thread scan every interval

    // Stutter detection
    ProbeMode probeMode = ProbeMode::Floating;
    int floatingProbes = 4;
    double floatingThresholdMs = 2.0;
    double perCpuThresholdMs = 4.0;   // > EEVDF base slice (~3 ms): normal fair-share waits aren't hitches
    double realtimeThresholdMs = 0.5;

    // Thermal
    double tjmaxC = 0;               // 0 = auto-detect from CPU model

    // Rule thresholds
    double highCpuCores = 0.9;       // a process using >= this many cores ...
    int highCpuSustainSec = 10;      // ... for this long is reported
    double cpuTempWarnC = 80;
    int findingRaiseAfter = 2;       // consecutive evaluations before a finding is shown
    int findingClearAfter = 5;       // consecutive clean evaluations before it clears

    // Recorder
    QString recordingsDir;           // empty = ~/.local/share/culprit/recordings

    static Settings load();
    void save() const;
    QString effectiveRecordingsDir() const;
};

} // namespace culprit
