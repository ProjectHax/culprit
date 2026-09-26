// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/widgets/DataTable.h"

#include "gui/Theme.h"
#include "gui/widgets/HeaderFit.h"

#include <QHeaderView>
#include <QPainter>

namespace culprit {

namespace {

// Sorts by the UserRole key when one is set.
class SortItem : public QTableWidgetItem {
public:
    bool operator<(const QTableWidgetItem& other) const override
    {
        const QVariant a = data(Qt::UserRole), b = other.data(Qt::UserRole);
        if (a.isValid() && b.isValid())
            return a.toDouble() < b.toDouble();
        return text().compare(other.text(), Qt::CaseInsensitive) < 0;
    }
};

} // namespace

DataTable::DataTable(const QStringList& headers, QWidget* parent) : QTableWidget(0, int(headers.size()), parent)
{
    setHorizontalHeaderLabels(headers);
    numeric_.assign(size_t(headers.size()), false);
    verticalHeader()->hide();
    verticalHeader()->setDefaultSectionSize(fontMetrics().height() + 6);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setAlternatingRowColors(true);
    setWordWrap(false);
    horizontalHeader()->setStretchLastSection(true);
    horizontalHeader()->setHighlightSections(false);
    setSortingEnabled(true);
    sortByColumn(0, Qt::AscendingOrder);
}

void DataTable::setNumericColumns(const std::vector<int>& cols)
{
    for (int c : cols)
        if (c >= 0 && c < int(numeric_.size()))
            numeric_[size_t(c)] = true;
}

void DataTable::setRows(const std::vector<Row>& rows)
{
    const bool sorting = isSortingEnabled();
    setSortingEnabled(false);
    const int oldRows = rowCount();
    setRowCount(int(rows.size()));
    // Fill with the model's signals blocked and announce one dataChanged for the
    // whole table: per-cell notifications make the view (and any auto-sizing
    // header) re-measure the table hundreds of times per refresh.
    const bool wasBlocked = model()->blockSignals(true);
    for (int r = 0; r < int(rows.size()); ++r) {
        for (int c = 0; c < columnCount(); ++c) {
            const Cell cell = c < int(rows[size_t(r)].size()) ? rows[size_t(r)][size_t(c)] : Cell();
            QTableWidgetItem* it = item(r, c);
            if (!it) {
                it = new SortItem;
                if (numeric_[size_t(c)])
                    it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                setItem(r, c, it);
            }
            it->setText(cell.text);
            it->setData(Qt::UserRole, cell.sort);
            it->setData(Qt::UserRole + 1, cell.id);
            it->setForeground(cell.color.isValid() ? QBrush(cell.color) : QBrush());
            it->setToolTip(cell.tooltip.isEmpty() ? (cell.text.size() > 40 ? cell.text : QString()) : cell.tooltip);
        }
    }
    model()->blockSignals(wasBlocked);
    if (!rows.empty())
        emit model()->dataChanged(model()->index(0, 0), model()->index(rowCount() - 1, columnCount() - 1));
    setSortingEnabled(sorting);
    // Size after sorting is back on: with it off, Qt leaves out the sort-arrow
    // space and the column titles end up clipped.
    if (!sized_ || (oldRows == 0 && !rows.empty())) {
        resizeColumnsToContents();
        HeaderFitter::fit(horizontalHeader());
        sized_ = !rows.empty();
    }
    viewport()->update();
}

void DataTable::paintEvent(QPaintEvent* e)
{
    QTableWidget::paintEvent(e);
    if (rowCount() == 0 && !empty_.isEmpty()) {
        QPainter p(viewport());
        p.setPen(theme::dimText(palette()));
        p.drawText(viewport()->rect().adjusted(12, 12, -12, -12), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, empty_);
    }
}

} // namespace culprit
