// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

namespace culprit {

enum class Severity { Info = 0, Warning = 1, Critical = 2 };

QString severityName(Severity s);

// Something (a process, IRQ line, CPU, device, ...) implicated in a finding or hitch.
struct Suspect {
    QString kind;       // "process", "thread", "irq", "softirq", "cpu", "disk", "gpu", "kernel", "cgroup", "self"
    QString label;      // human readable: "firefox (pid 4242)", "IRQ 120 nvidia"
    int pid = 0;        // for process/thread suspects
    double score = 0;   // 0..1 relative confidence / contribution
    QString evidence;   // one line of supporting numbers
};

// A diagnosis produced by the rule engine. `id` is stable across evaluations
// (rule + subject), so the same issue is tracked over time rather than re-raised.
struct Finding {
    QString id;
    QString rule;
    Severity severity = Severity::Info;
    QString title;
    QString detail;
    QString advice;
    QStringList evidence;
    std::vector<Suspect> suspects;
    int64_t firstSeenNs = 0;
    int64_t lastSeenNs = 0;
    qint64 firstSeenWallMs = 0;
    qint64 lastSeenWallMs = 0;
    bool active = true;
};

} // namespace culprit
