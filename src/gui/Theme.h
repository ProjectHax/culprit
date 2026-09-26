// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/model/Finding.h"

#include <QColor>
#include <QPalette>

namespace culprit::theme {

bool isDark(const QPalette& p);

// Series colours chosen to stay distinguishable on light and dark palettes.
QColor series(int i);
inline QColor blue() { return series(0); }
inline QColor orange() { return series(1); }
inline QColor green() { return series(2); }
inline QColor purple() { return series(3); }
inline QColor red() { return QColor(0xe5, 0x48, 0x4d); }
inline QColor amber() { return QColor(0xf5, 0x9e, 0x0b); }

QColor severity(Severity s);
QColor heat(double t);                      // 0..1 -> cool..hot
QColor cardBackground(const QPalette& p);
QColor gridLine(const QPalette& p);
QColor dimText(const QPalette& p);

} // namespace culprit::theme
