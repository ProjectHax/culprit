// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include <QColor>
#include <QTableWidget>
#include <QVariant>

#include <vector>

namespace culprit {

// A read-only table that is refreshed wholesale each frame. Items are reused,
// numeric columns sort numerically, and the user's sort order is kept.
class DataTable : public QTableWidget {
    Q_OBJECT
public:
    struct Cell {
        QString text;
        QVariant sort;          // numeric sort key (defaults to text)
        QColor color;           // foreground, invalid = default
        QString tooltip;
        QVariant id;            // caller's row identifier (Qt::UserRole + 1)
        Cell() = default;
        Cell(QString t) : text(std::move(t)) {}
        Cell(const char* t) : text(QString::fromUtf8(t)) {}
        Cell(QString t, double s, QColor c = {}) : text(std::move(t)), sort(s), color(c) {}
    };
    using Row = std::vector<Cell>;

    DataTable(const QStringList& headers, QWidget* parent = nullptr);

    void setNumericColumns(const std::vector<int>& cols);
    void setRows(const std::vector<Row>& rows);
    void setEmptyText(const QString& text) { empty_ = text; }

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    std::vector<bool> numeric_;
    QString empty_;
    bool sized_ = false;
};

} // namespace culprit
