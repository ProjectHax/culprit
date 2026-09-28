// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/widgets/LayoutBarrier.h"

#include <QEvent>

namespace culprit {

LayoutBarrier::LayoutBarrier(QWidget* content, QWidget* parent)
    : QWidget(parent), content_(content), hint_(content->sizeHint()), min_(content->minimumSizeHint())
{
    content_->setParent(this);
    setSizePolicy(content_->sizePolicy());
}

bool LayoutBarrier::event(QEvent* e)
{
    // The content changed its size hints (it has already re-laid out itself).
    if (e->type() == QEvent::LayoutRequest) {
        hint_ = content_->sizeHint();
        const QSize min = content_->minimumSizeHint();
        if (min != min_) {
            min_ = min;
            updateGeometry();
        }
        return true;
    }
    return QWidget::event(e);
}

void LayoutBarrier::resizeEvent(QResizeEvent*)
{
    content_->setGeometry(rect());
}

} // namespace culprit
