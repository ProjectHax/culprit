// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include <QWidget>

namespace culprit {

// Holds a tab page and stops its layout updates at the page.
//
// Any text change inside a page (a label updated every second) makes Qt
// re-lay out every ancestor up to the window, and a QTabWidget repaints its
// whole area whenever it is asked to re-lay out, so each refresh repainted
// the entire window. The page still re-lays out its own contents; its
// parents only hear about it when its minimum size changes.
class LayoutBarrier : public QWidget {
public:
    explicit LayoutBarrier(QWidget* content, QWidget* parent = nullptr);

    QWidget* content() const { return content_; }
    QSize sizeHint() const override { return hint_; }
    QSize minimumSizeHint() const override { return min_; }

protected:
    bool event(QEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    QWidget* content_;
    QSize hint_, min_;
};

} // namespace culprit
