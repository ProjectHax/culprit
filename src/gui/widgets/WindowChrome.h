// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/Settings.h"

#include <QDialog>
#include <QPointer>
#include <QString>

class QVBoxLayout;
class QWindow;

namespace culprit {

class TitleBar;

// Whether Culprit should draw its own title bar, and why.
//
// On Wayland compositors that don't offer server-side decorations (GNOME/Mutter,
// Weston), Qt falls back to drawing a title bar itself — and that fallback is
// drawn in physical pixels, so it ignores QT_SCALE_FACTOR and looks too small
// next to a scaled UI. There Culprit draws its own, which scales with the rest.
struct TitleBarDecision {
    bool integrated = false;
    QString reason;
};
TitleBarDecision decideTitleBar(TitleBarMode mode);

// Frameless-window support for a top-level widget: resizing from the window
// edges and moving are handed to the compositor (startSystemResize/Move), so
// snapping, tiling and multi-monitor behaviour stay native.
class WindowChrome : public QObject {
    Q_OBJECT
public:
    explicit WindowChrome(QWidget* window);
    void setActive(bool active);
    bool isActive() const { return active_; }

    // Global switch read by dialogs created later.
    static bool integratedTitleBars();
    static void setIntegratedTitleBars(bool on);

    // 1px outline around a frameless window (skipped when maximized).
    static void paintOutline(QWidget* w);

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void attach();
    Qt::Edges edgesAt(const QPointF& pos) const;
    void setEdgeCursor(Qt::Edges edges);

    QWidget* window_;
    QPointer<QWindow> handle_;
    Qt::Edges cursorEdges_;
    bool overrideCursor_ = false;
    bool active_ = false;
};

// A QDialog that gets Culprit's title bar when integrated title bars are on.
// Put the dialog's content into body().
class ChromeDialog : public QDialog {
    Q_OBJECT
public:
    explicit ChromeDialog(QWidget* parent, const QString& title);
    QVBoxLayout* body() const { return body_; }

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    QVBoxLayout* body_ = nullptr;
    WindowChrome* chrome_ = nullptr;
    bool integrated_ = false;
};

} // namespace culprit
