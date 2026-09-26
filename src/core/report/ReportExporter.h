// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/model/Frame.h"
#include "core/record/RecordingReader.h"

#include <QJsonObject>
#include <QString>

#include <vector>

namespace culprit {

// Shareable diagnostic reports (plain text or JSON) for the live system or a recording.
namespace Report {

QString snapshotText(const Frame& f, const std::vector<Hitch>& hitches);
QJsonObject snapshotJson(const Frame& f, const std::vector<Hitch>& hitches);

QString recordingText(const Recording& r);
QJsonObject recordingJson(const Recording& r);

// Writes text or JSON depending on the file extension (.json -> JSON).
bool saveSnapshot(const QString& path, const Frame& f, const std::vector<Hitch>& hitches, QString* error);
bool saveRecording(const QString& path, const Recording& r, QString* error);

} // namespace Report
} // namespace culprit
