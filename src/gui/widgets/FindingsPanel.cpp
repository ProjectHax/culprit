// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/widgets/FindingsPanel.h"

#include "core/Format.h"
#include "gui/Theme.h"

#include <QCheckBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace culprit {

namespace {

QIcon severityIcon(Severity s, bool active, qreal dpr)
{
    QPixmap pm(QSize(14, 14) * dpr);   // device pixels, so it stays sharp when scaled
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QColor c = theme::severity(s);
    if (!active)
        c.setAlpha(90);
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QRectF(1.5, 1.5, 11, 11));
    return QIcon(pm);
}

} // namespace

QString findingHtml(const Finding& f, const QPalette& pal)
{
    const QString dim = theme::dimText(pal).name(QColor::HexArgb);
    QString h = QStringLiteral("<h3 style='margin:0 0 4px 0'><span style='color:%1'>●</span> %2</h3>")
                    .arg(theme::severity(f.severity).name(), f.title.toHtmlEscaped());
    const double age = double(f.lastSeenNs - f.firstSeenNs) / 1e9;
    h += QStringLiteral("<div style='color:%1'>%2 · first seen %3%4%5</div>")
             .arg(dim, severityName(f.severity), fmt::wallTime(f.firstSeenWallMs).left(8))
             .arg(age >= 1 ? QObject::tr(" · for %1").arg(fmt::duration(age)) : QString())
             .arg(f.active ? QString() : QObject::tr(" · <b>cleared</b> at %1").arg(fmt::wallTime(f.lastSeenWallMs).left(8)));
    if (!f.detail.isEmpty())
        h += QStringLiteral("<p>%1</p>").arg(f.detail.toHtmlEscaped());
    if (!f.suspects.empty()) {
        h += QObject::tr("<p style='margin-bottom:2px'><b>Likely culprits</b></p><table cellspacing=0 cellpadding=2>");
        for (const Suspect& s : f.suspects) {
            const QString label = s.pid > 0 ? QStringLiteral("<a href='pid:%1'>%2</a>").arg(s.pid).arg(s.label.toHtmlEscaped())
                                            : s.label.toHtmlEscaped();
            const int bar = int(std::clamp(s.score, 0.0, 1.0) * 60);
            h += QStringLiteral("<tr><td><span style='background:%1'>%2</span></td><td style='padding-left:6px'>%3</td>"
                                "<td style='padding-left:10px;color:%4'>%5</td></tr>")
                     .arg(theme::severity(f.severity).name(), QString(std::max(1, bar / 6), QChar(0x2002)), label, dim,
                          s.evidence.toHtmlEscaped());
        }
        h += QStringLiteral("</table>");
    }
    if (!f.evidence.isEmpty()) {
        h += QObject::tr("<p style='margin-bottom:2px'><b>Evidence</b></p><ul style='margin-top:0'>");
        for (const QString& e : f.evidence)
            h += QStringLiteral("<li>%1</li>").arg(e.toHtmlEscaped());
        h += QStringLiteral("</ul>");
    }
    if (!f.advice.isEmpty())
        h += QObject::tr("<p><b>What to do:</b> %1</p>").arg(f.advice.toHtmlEscaped());
    return h;
}

FindingsPanel::FindingsPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 4, 0, 0);
    auto* top = new QHBoxLayout;
    header_ = new QLabel;
    QFont hf = header_->font();
    hf.setBold(true);
    hf.setPointSizeF(hf.pointSizeF() * 1.1);
    header_->setFont(hf);
    showCleared_ = new QCheckBox(tr("Show cleared"));
    showCleared_->setChecked(true);
    top->addWidget(header_, 1);
    top->addWidget(showCleared_);
    v->addLayout(top);

    auto* split = new QSplitter(Qt::Horizontal);
    list_ = new QListWidget;
    list_->setUniformItemSizes(true);
    list_->setWordWrap(false);
    details_ = new QTextBrowser;
    details_->setOpenLinks(false);
    details_->setPlaceholderText(tr("Select a finding."));
    split->addWidget(list_);
    split->addWidget(details_);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    v->addWidget(split, 1);

    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* cur) {
        selectedId_ = cur ? cur->data(Qt::UserRole).toString() : QString();
        autoSelected_.clear();   // the user picked one: stop following the top finding
        const Finding* f = nullptr;
        for (const Finding& x : findings_)
            if (x.id == selectedId_)
                f = &x;
        showDetails(f);
    });
    connect(details_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.scheme() == QLatin1String("pid"))
            emit processActivated(url.path().toInt());
    });
    connect(showCleared_, &QCheckBox::toggled, this, [this] { setFindings(std::vector<Finding>(findings_)); });
}

void FindingsPanel::setFindings(const std::vector<Finding>& findings)
{
    findings_ = findings;
    int active = 0, warn = 0, crit = 0;
    for (const Finding& f : findings_) {
        if (!f.active)
            continue;
        ++active;
        warn += f.severity == Severity::Warning;
        crit += f.severity == Severity::Critical;
    }
    QStringList counts;
    if (crit)
        counts << tr("%n critical", nullptr, crit);
    if (warn)
        counts << tr("%n warning(s)", nullptr, warn);
    if (active - crit - warn)
        counts << tr("%n info", nullptr, active - crit - warn);
    header_->setText(active == 0 ? tr("Diagnosis · all clear") : tr("Diagnosis · %1").arg(counts.join(QStringLiteral(" · "))));

    // Rebuild the list in place, keeping the selection.
    list_->blockSignals(true);
    int row = 0;
    int selectRow = -1;
    for (const Finding& f : findings_) {
        if (!f.active && !showCleared_->isChecked())
            continue;
        QListWidgetItem* it = row < list_->count() ? list_->item(row) : new QListWidgetItem(list_);
        it->setIcon(severityIcon(f.severity, f.active, devicePixelRatioF()));
        it->setText(f.active ? f.title : tr("%1  (cleared)").arg(f.title));
        it->setData(Qt::UserRole, f.id);
        it->setForeground(f.active ? palette().color(QPalette::Text) : theme::dimText(palette()));
        it->setToolTip(f.detail);
        if (f.id == selectedId_)
            selectRow = row;
        ++row;
    }
    while (list_->count() > row)
        delete list_->takeItem(list_->count() - 1);
    // Nothing chosen yet: show the most important finding.
    if (row > 0 && (selectedId_.isEmpty() || !autoSelected_.isEmpty())) {
        selectRow = 0;
        selectedId_ = autoSelected_ = list_->item(0)->data(Qt::UserRole).toString();
    }
    list_->setCurrentRow(selectRow);
    list_->blockSignals(false);

    const Finding* sel = nullptr;
    for (const Finding& f : findings_)
        if (f.id == selectedId_)
            sel = &f;
    if (sel || !selectedId_.isEmpty())
        showDetails(sel);
}

void FindingsPanel::showDetails(const Finding* f)
{
    const int scroll = details_->verticalScrollBar() ? details_->verticalScrollBar()->value() : 0;
    details_->setHtml(f ? findingHtml(*f, palette()) : QString());
    if (details_->verticalScrollBar())
        details_->verticalScrollBar()->setValue(scroll);
}

} // namespace culprit
