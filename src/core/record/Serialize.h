// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/model/Frame.h"

#include <QJsonObject>

namespace culprit {

// JSON (de)serialisation shared by the recorder, recording reader and reports.
QJsonObject toJson(const Suspect& s);
QJsonObject toJson(const Finding& f);
QJsonObject toJson(const Hitch& h);
QJsonObject toJson(const ProcSample& p);

Suspect suspectFromJson(const QJsonObject& o);
Finding findingFromJson(const QJsonObject& o);
Hitch hitchFromJson(const QJsonObject& o);

QString hitchClassKey(HitchClass c);          // stable identifier, e.g. "runqueue"
HitchClass hitchClassFromKey(const QString& k);

} // namespace culprit
