// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/model/Frame.h"

#include <QWidget>

namespace culprit {

// Base for main-window tabs. Every frame is delivered to every tab (so history
// keeps accumulating), but expensive view refreshes only happen while the tab
// is visible — and once more when it becomes visible.
class TabPage : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;

    void onFrame(const FramePtr& frame)
    {
        last_ = frame;
        accumulate(*frame);
        if (isVisible())
            render(*frame);
    }
    const FramePtr& lastFrame() const { return last_; }

protected:
    // Always called: keep histories up to date.
    virtual void accumulate(const Frame&) {}
    // Called only while visible: refresh the view.
    virtual void render(const Frame&) = 0;

    void showEvent(QShowEvent* e) override
    {
        QWidget::showEvent(e);
        if (last_)
            render(*last_);
    }

private:
    FramePtr last_;
};

} // namespace culprit
