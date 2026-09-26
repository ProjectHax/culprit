// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/tabs/RecordingsTab.h"

#include "core/Format.h"
#include "core/report/ReportExporter.h"
#include "gui/Dialogs.h"
#include "gui/Theme.h"
#include "gui/widgets/DataTable.h"
#include "gui/widgets/FindingsPanel.h"
#include "gui/widgets/HitchDetail.h"
#include "gui/widgets/TimelineView.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace culprit {

RecordingsTab::RecordingsTab(const QString& dir, QWidget* parent) : TabPage(parent), dir_(dir)
{
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(6, 6, 6, 6);
    auto* split = new QSplitter(Qt::Horizontal);
    outer->addWidget(split);

    // Left: recordings list
    auto* left = new QWidget;
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    auto* lbl = new QLabel(tr("<b>Recordings</b>"));
    files_ = new QListWidget;
    auto* btns = new QHBoxLayout;
    auto* refresh = new QPushButton(tr("Refresh"));
    auto* open = new QPushButton(tr("Open file…"));
    btns->addWidget(refresh);
    btns->addWidget(open);
    files_->setToolTip(tr("Record with the Record button, or in the background with\n"
                          "culprit --record  /  systemctl --user start culprit-recorder"));
    lv->addWidget(lbl);
    lv->addWidget(files_, 1);
    lv->addLayout(btns);
    split->addWidget(left);

    // Right: the selected recording
    auto* right = new QWidget;
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    auto* head = new QHBoxLayout;
    header_ = new QLabel(tr("Select a recording."));
    header_->setWordWrap(true);
    header_->setTextFormat(Qt::RichText);
    exportText_ = new QPushButton(tr("Export text report…"));
    exportJson_ = new QPushButton(tr("Export JSON…"));
    exportText_->setEnabled(false);
    exportJson_->setEnabled(false);
    head->addWidget(header_, 1);
    head->addWidget(exportText_);
    head->addWidget(exportJson_);
    rv->addLayout(head);

    auto* vsplit = new QSplitter(Qt::Vertical);
    top_ = new TimelineView;
    top_->setSeries({{tr("CPU busy %"), theme::blue(), true, false, {}}, {tr("CPU temperature °C"), theme::red(), false, true, {}}});
    top_->setYUnit(QStringLiteral("%"));
    top_->setRightUnit(QStringLiteral("°C"));
    top_->setYRange(0, 100, false);
    top_->setRightYRange(20, 100, true);
    top_->setEmptyText(tr("No recording loaded."));
    bottom_ = new TimelineView;
    bottom_->setSeries({{tr("worst wakeup delay (ms)"), theme::orange(), true, false, {}}, {tr("CPU package W"), theme::green(), false, true, {}}});
    bottom_->setYUnit(tr("ms"));
    bottom_->setRightUnit(QStringLiteral("W"));
    bottom_->setYRange(0, 4, true);
    bottom_->setRightYRange(0, 50, true);
    vsplit->addWidget(top_);
    vsplit->addWidget(bottom_);

    auto* lower = new QSplitter(Qt::Horizontal);
    lists_ = new QTabWidget;
    hitches_ = new DataTable({tr("Time"), tr("Delay"), tr("Cause"), tr("CPU"), tr("Explanation")});
    hitches_->setNumericColumns({1, 3});
    hitches_->setEmptyText(tr("No hitches in this recording."));
    findings_ = new DataTable({tr("Since"), tr("For"), tr("Severity"), tr("Finding")});
    findings_->setEmptyText(tr("No findings in this recording."));
    lists_->addTab(hitches_, tr("Hitches"));
    lists_->addTab(findings_, tr("Findings"));
    detail_ = new QTextBrowser;
    detail_->setOpenLinks(false);
    lower->addWidget(lists_);
    lower->addWidget(detail_);
    lower->setStretchFactor(0, 3);
    lower->setStretchFactor(1, 2);
    vsplit->addWidget(lower);
    vsplit->setStretchFactor(0, 1);
    vsplit->setStretchFactor(1, 1);
    vsplit->setStretchFactor(2, 2);
    rv->addWidget(vsplit, 1);
    split->addWidget(right);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({280, 1200});

    connect(refresh, &QPushButton::clicked, this, &RecordingsTab::refresh);
    connect(open, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Open recording"), dir_, tr("Culprit recordings (*.jsonl);;All files (*)"));
        if (!f.isEmpty())
            openFile(f);
    });
    connect(files_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* it) {
        if (it)
            openFile(it->data(Qt::UserRole).toString());
    });
    connect(top_, &TimelineView::markerClicked, this, &RecordingsTab::showHitch);
    connect(bottom_, &TimelineView::markerClicked, this, &RecordingsTab::showHitch);
    connect(hitches_, &DataTable::currentCellChanged, this, [this](int row) {
        if (auto* it = row >= 0 ? hitches_->item(row, 0) : nullptr)
            showHitch(it->data(Qt::UserRole + 1).toLongLong());
    });
    connect(findings_, &DataTable::currentCellChanged, this, [this](int row) {
        if (auto* it = row >= 0 ? findings_->item(row, 0) : nullptr)
            showFinding(it->data(Qt::UserRole + 1).toInt());
    });
    connect(exportText_, &QPushButton::clicked, this, [this] { exportReport(false); });
    connect(exportJson_, &QPushButton::clicked, this, [this] { exportReport(true); });
}

void RecordingsTab::setDirectory(const QString& dir)
{
    dir_ = dir;
    refresh();
}

void RecordingsTab::showEvent(QShowEvent* e)
{
    TabPage::showEvent(e);
    refresh();
}

void RecordingsTab::refresh()
{
    const QString current = files_->currentItem() ? files_->currentItem()->data(Qt::UserRole).toString() : QString();
    files_->blockSignals(true);
    files_->clear();
    const QFileInfoList list = QDir(dir_).entryInfoList({QStringLiteral("*.jsonl")}, QDir::Files, QDir::Time);
    for (const QFileInfo& fi : list) {
        auto* it = new QListWidgetItem(QStringLiteral("%1\n%2 · %3")
                                           .arg(fi.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                                           .arg(fmt::bytes(double(fi.size())), fi.fileName()),
                                       files_);
        it->setData(Qt::UserRole, fi.absoluteFilePath());
        it->setToolTip(fi.absoluteFilePath());
        if (fi.absoluteFilePath() == current)
            files_->setCurrentItem(it);
    }
    files_->blockSignals(false);
}

bool RecordingsTab::openFile(const QString& path)
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString err;
    Recording r;
    const bool ok = readRecording(path, r, &err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        header_->setText(tr("<b>%1</b>: %2").arg(QFileInfo(path).fileName().toHtmlEscaped(), err.toHtmlEscaped()));
        loaded_ = false;
        exportText_->setEnabled(false);
        exportJson_->setEnabled(false);
        return false;
    }
    rec_ = std::move(r);
    loaded_ = true;
    showRecording();
    return true;
}

void RecordingsTab::showRecording()
{
    const Recording& r = rec_;
    exportText_->setEnabled(true);
    exportJson_->setEnabled(true);
    double worst = 0;
    for (const Hitch& h : r.hitches)
        worst = std::max(worst, h.maxOvershootMs);
    header_->setText(tr("<b>%1</b> — %2 on %3 · %4 · %5 hitch(es), worst %6 · %7 finding(s)")
                         .arg(QFileInfo(r.path).fileName().toHtmlEscaped(),
                              QDateTime::fromMSecsSinceEpoch(r.startMs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                              r.header.value(QStringLiteral("host")).toString().toHtmlEscaped(),
                              fmt::duration(double(r.endMs - r.startMs) / 1000.0))
                         .arg(r.hitches.size())
                         .arg(fmt::ms(worst))
                         .arg(r.findings.size()));

    const double span = std::max(1.0, double(r.endMs - r.startMs) / 1000.0);
    auto x = [&](qint64 wall) { return double(wall - r.startMs) / 1000.0; };
    std::vector<QPointF> cpu, temp, lat, watts;
    for (const RecordingPoint& p : r.points) {
        cpu.emplace_back(x(p.wallMs), p.cpu);
        if (p.tctl > -100)
            temp.emplace_back(x(p.wallMs), p.tctl);
        if (p.latMs >= 0)
            lat.emplace_back(x(p.wallMs), p.latMs);
        if (p.pkgW >= 0)
            watts.emplace_back(x(p.wallMs), p.pkgW);
    }
    std::vector<TimelineView::Marker> markers;
    for (const Hitch& h : r.hitches)
        markers.push_back({x(h.wallMs), hitchColor(h.cls), QStringLiteral("%1\n%2").arg(fmt::wallTime(h.wallMs), h.summary), qint64(h.id)});
    for (TimelineView* tv : {top_, bottom_}) {
        tv->setWallClockBase(r.startMs);
        tv->setXRange(0, span);
        tv->setMarkers(markers);
    }
    top_->setPoints(0, std::move(cpu));
    top_->setPoints(1, std::move(temp));
    bottom_->setPoints(0, std::move(lat));
    bottom_->setPoints(1, std::move(watts));

    std::vector<DataTable::Row> rows;
    for (const Hitch& h : r.hitches) {
        DataTable::Cell t(fmt::wallTime(h.wallMs), double(h.wallMs));
        t.id = qint64(h.id);
        DataTable::Cell cls(hitchClassName(h.cls));
        cls.color = hitchColor(h.cls);
        rows.push_back({t, DataTable::Cell(fmt::ms(h.maxOvershootMs), h.maxOvershootMs), cls,
                        DataTable::Cell(QString::number(h.worstCpu), h.worstCpu), DataTable::Cell(h.summary)});
    }
    hitches_->setRows(rows);
    rows.clear();
    for (size_t i = 0; i < r.findings.size(); ++i) {
        const Finding& f = r.findings[i];
        DataTable::Cell t(fmt::wallTime(f.firstSeenWallMs).left(8), double(f.firstSeenWallMs));
        t.id = int(i);
        DataTable::Cell sev(severityName(f.severity), double(f.severity), theme::severity(f.severity));
        rows.push_back({t, DataTable::Cell(fmt::duration(double(f.lastSeenWallMs - f.firstSeenWallMs) / 1000.0),
                                           double(f.lastSeenWallMs - f.firstSeenWallMs)),
                        sev, DataTable::Cell(f.title)});
    }
    findings_->setRows(rows);
    lists_->setTabText(0, tr("Hitches (%1)").arg(r.hitches.size()));
    lists_->setTabText(1, tr("Findings (%1)").arg(r.findings.size()));
    detail_->setHtml(tr("<p>Select a hitch or finding.</p>"));
}

void RecordingsTab::showHitch(qint64 id)
{
    top_->setSelectedMarker(id);
    bottom_->setSelectedMarker(id);
    for (const Hitch& h : rec_.hitches)
        if (qint64(h.id) == id) {
            detail_->setHtml(hitchHtml(h, palette()));
            lists_->setCurrentIndex(0);
            return;
        }
}

void RecordingsTab::showFinding(int index)
{
    if (index >= 0 && index < int(rec_.findings.size()))
        detail_->setHtml(findingHtml(rec_.findings[size_t(index)], palette()));
}

void RecordingsTab::exportReport(bool json)
{
    if (!loaded_)
        return;
    const QString base = QFileInfo(rec_.path).completeBaseName() + (json ? QStringLiteral("-report.json") : QStringLiteral("-report.txt"));
    const QString path = QFileDialog::getSaveFileName(this, tr("Export report"), QDir::home().filePath(base),
                                                      json ? tr("JSON (*.json)") : tr("Text (*.txt)"));
    if (path.isEmpty())
        return;
    QString err;
    if (!Report::saveRecording(path, rec_, &err))
        Dialogs::warning(this, tr("Export failed"), err);
    else
        emit statusMessage(tr("Report written to %1").arg(path));
}

} // namespace culprit
