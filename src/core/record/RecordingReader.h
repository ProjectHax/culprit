// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/model/Frame.h"

#include <QJsonObject>
#include <QString>

#include <vector>

namespace culprit {

struct RecordingPoint {
    qint64 wallMs = 0;
    double cpu = 0, load = 0, rq = 0, mem = 0;
    double runnable = -1, blocked = -1;
    double tctl = -1000, pkgW = -1, gpuT = -1, gpuW = -1, gpuU = -1, latMs = -1, disk = 0;
    double swapin = 0, majflt = 0;
    struct Top {
        QString comm;
        int pid = 0;
        double cpu = 0, watts = 0;
    };
    std::vector<Top> top;
};

struct Recording {
    QString path;
    QJsonObject header;
    std::vector<RecordingPoint> points;
    std::vector<Hitch> hitches;
    std::vector<Finding> findings;   // one entry per episode (raised .. cleared)
    qint64 startMs = 0, endMs = 0;
};

bool readRecording(const QString& path, Recording& out, QString* error);

} // namespace culprit
