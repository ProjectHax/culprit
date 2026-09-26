// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/model/Hitch.h"

#include <QPalette>
#include <QString>

namespace culprit {

// Rich-text explanation of one hitch (used by the Stutter and Recordings tabs).
QString hitchHtml(const Hitch& h, const QPalette& pal);
QColor hitchColor(HitchClass c);

} // namespace culprit
