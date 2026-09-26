// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/widgets/HeaderFit.h"

#include <QApplication>
#include <QEvent>
#include <QHeaderView>

namespace culprit {

void HeaderFitter::install()
{
    static HeaderFitter* fitter = nullptr;
    if (!fitter) {
        fitter = new HeaderFitter;
        fitter->setParent(qApp);
        qApp->installEventFilter(fitter);
    }
}

void HeaderFitter::fit(QHeaderView* h)
{
    if (h->orientation() != Qt::Horizontal)
        return;
    const int last = h->count() - 1;
    for (int i = 0; i < h->count(); ++i) {
        if (h->isSectionHidden(i) || h->sectionResizeMode(i) == QHeaderView::Stretch ||
            h->sectionResizeMode(i) == QHeaderView::ResizeToContents)
            continue;
        if (i == last && h->stretchLastSection())
            continue;   // fills the remaining width anyway
        const int need = h->sectionSizeHint(i);
        if (h->sectionSize(i) < need)
            h->resizeSection(i, need);
    }
}

bool HeaderFitter::eventFilter(QObject* o, QEvent* e)
{
    switch (e->type()) {
    case QEvent::Show:
    case QEvent::FontChange:
    case QEvent::StyleChange:
        if (auto* h = qobject_cast<QHeaderView*>(o)) {
            // Show fires on every tab switch: only fit the first time, unless the
            // font or style changed (which can make titles wider).
            if (e->type() != QEvent::Show || !h->property("culpritFitted").toBool()) {
                h->setProperty("culpritFitted", true);
                fit(h);
            }
        }
        break;
    default:
        break;
    }
    return QObject::eventFilter(o, e);
}

} // namespace culprit
