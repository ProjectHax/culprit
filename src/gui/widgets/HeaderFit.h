// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include <QObject>

class QHeaderView;

namespace culprit {

// Makes sure every column is at least wide enough to show its title (text,
// padding and sort-arrow space for the current font and style), for every
// table and tree in the application. Columns are only ever widened, and only
// when a header is first shown or its font/style changes, so manual resizing
// still sticks.
class HeaderFitter : public QObject {
    Q_OBJECT
public:
    static void install();
    static void fit(QHeaderView* header);

protected:
    bool eventFilter(QObject* o, QEvent* e) override;
};

} // namespace culprit
