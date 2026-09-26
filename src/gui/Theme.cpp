// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/Theme.h"

#include "gui/ThemeManager.h"

#include <algorithm>

namespace culprit::theme {

bool isDark(const QPalette&) { return ThemeManager::instance().isDark(); }

QColor series(int i)
{
    static const QColor colors[] = {
        QColor(0x3b, 0x82, 0xf6),   // blue
        QColor(0xf9, 0x73, 0x16),   // orange
        QColor(0x10, 0xb9, 0x81),   // green
        QColor(0xa8, 0x55, 0xf7),   // purple
        QColor(0xec, 0x48, 0x99),   // pink
        QColor(0x06, 0xb6, 0xd4),   // cyan
    };
    return colors[size_t(i) % std::size(colors)];
}

QColor severity(Severity s)
{
    switch (s) {
    case Severity::Info: return QColor(0x3b, 0x82, 0xf6);
    case Severity::Warning: return QColor(0xf5, 0x9e, 0x0b);
    case Severity::Critical: return QColor(0xe5, 0x48, 0x4d);
    }
    return {};
}

QColor heat(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    // blue -> green -> yellow -> red
    struct Stop {
        double t;
        QColor c;
    };
    static const Stop stops[] = {{0.0, QColor(0x25, 0x63, 0xeb)},
                                 {0.35, QColor(0x10, 0xb9, 0x81)},
                                 {0.65, QColor(0xf5, 0x9e, 0x0b)},
                                 {1.0, QColor(0xdc, 0x26, 0x26)}};
    for (size_t i = 1; i < std::size(stops); ++i) {
        if (t <= stops[i].t) {
            const double f = (t - stops[i - 1].t) / (stops[i].t - stops[i - 1].t);
            const QColor& a = stops[i - 1].c;
            const QColor& b = stops[i].c;
            return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * f),
                                    float(a.greenF() + (b.greenF() - a.greenF()) * f),
                                    float(a.blueF() + (b.blueF() - a.blueF()) * f));
        }
    }
    return stops[std::size(stops) - 1].c;
}

// These read the active ThemeManager tokens so custom-painted widgets match
// the stylesheet exactly in both light and dark mode.
QColor cardBackground(const QPalette&) { return ThemeManager::instance().tokens().card; }

QColor gridLine(const QPalette&)
{
    const ThemeTokens& t = ThemeManager::instance().tokens();
    return t.dark ? t.border.lighter(112) : t.border;
}

QColor dimText(const QPalette&) { return ThemeManager::instance().tokens().textDim; }

} // namespace culprit::theme
