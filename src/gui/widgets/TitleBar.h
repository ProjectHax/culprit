// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include <QAbstractButton>
#include <QWidget>

class QAction;
class QHBoxLayout;
class QLabel;

namespace culprit {

// Minimise / maximise / close button, drawn with vector glyphs so it is crisp
// at any scale factor.
class WindowButton : public QAbstractButton {
    Q_OBJECT
public:
    enum Kind { Minimize, Maximize, Restore, Close };
    explicit WindowButton(Kind kind, QWidget* parent = nullptr);
    void setKind(Kind k);
    QSize sizeHint() const override { return {34, 28}; }

protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

private:
    Kind kind_;
};

// Culprit's own title bar ("header bar"): icon, title, optional toolbar
// actions and window buttons. Drag to move, double-click to maximise,
// right-click for the window menu. Everything is a normal widget, so it scales
// with the rest of the UI.
class TitleBar : public QWidget {
    Q_OBJECT
public:
    enum Button { Minimize = 1, Maximize = 2, Close = 4, All = Minimize | Maximize | Close };
    TitleBar(QWidget* window, int buttons = All, QWidget* parent = nullptr);

    void addAction(QAction* action);
    void addSeparator();

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void toggleMaximized();
    void syncState();

    QWidget* window_;
    int buttons_;
    QLabel* icon_;
    QLabel* title_;
    QHBoxLayout* actions_;
    WindowButton* min_ = nullptr;
    WindowButton* max_ = nullptr;
    WindowButton* close_ = nullptr;
};

} // namespace culprit
