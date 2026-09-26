// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/Settings.h"

#include <QColor>
#include <QObject>
#include <QPalette>

namespace culprit {

class SystemAppearance;

// Colour tokens for one theme. Widgets read these (via theme:: helpers) for
// custom painting; the QPalette and stylesheet are generated from them too.
struct ThemeTokens {
    bool dark = false;
    QColor window, surface, surfaceAlt, card, border, borderStrong;
    QColor text, textDim, textFaint;
    QColor accent, accentHover, accentText, accentSoft, selection;
    QColor hover, pressed, titleBar, titleBarInactive;
    QColor tooltipBg, tooltipText, scrollHandle, scrollHandleHover;
    QColor danger, warning, success;
};

// Applies Culprit's light/dark theme (Fusion + palette + stylesheet) and keeps
// it in sync with the desktop when the mode is Automatic.
class ThemeManager : public QObject {
    Q_OBJECT
public:
    static ThemeManager& instance();   // create after QApplication

    void setMode(ThemeMode mode);
    ThemeMode mode() const { return mode_; }
    bool isDark() const { return tokens_.dark; }
    const ThemeTokens& tokens() const { return tokens_; }
    bool systemPrefersDark() const;
    // "dark, from desktop portal (color-scheme)" — for the settings dialog
    QString systemDescription() const;

signals:
    void themeChanged();

private:
    ThemeManager();
    void apply();
    bool eventFilter(QObject* o, QEvent* e) override;

    static ThemeTokens lightTokens();
    static ThemeTokens darkTokens();
    static QPalette paletteFor(const ThemeTokens& t);
    static QString styleSheetFor(const ThemeTokens& t);

    ThemeMode mode_ = ThemeMode::Auto;
    ThemeTokens tokens_;
    SystemAppearance* system_ = nullptr;
    bool applied_ = false;
    bool appliedDark_ = false;
};

} // namespace culprit
