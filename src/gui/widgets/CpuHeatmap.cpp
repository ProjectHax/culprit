// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/widgets/CpuHeatmap.h"

#include "core/Format.h"
#include "gui/Theme.h"

#include <QHelpEvent>
#include <QPainter>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace culprit {

namespace {
constexpr int kCellW = 74;
constexpr int kCellH = 46;
constexpr int kGap = 4;
} // namespace

CpuHeatmap::CpuHeatmap(QWidget* parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setMouseTracking(true);
}

void CpuHeatmap::setData(const SystemSample& sys)
{
    const size_t before = cpus_.size();
    cpus_ = sys.cpus;
    maxFreq_ = std::max(sys.policy.hwMaxMHz, 1.0);
    if (before != cpus_.size())
        updateGeometry();
    update();
}

int CpuHeatmap::columns(int width) const { return std::max(1, (width + kGap) / (kCellW + kGap)); }

QSize CpuHeatmap::sizeHint() const { return {8 * (kCellW + kGap), heightForWidth(8 * (kCellW + kGap))}; }

int CpuHeatmap::heightForWidth(int w) const
{
    const int n = std::max<int>(1, int(cpus_.size()));
    const int rows = (n + columns(w) - 1) / columns(w);
    return rows * (kCellH + kGap);
}

int CpuHeatmap::cellAt(const QPoint& pos) const
{
    const int cols = columns(width());
    const int c = pos.x() / (kCellW + kGap);
    const int r = pos.y() / (kCellH + kGap);
    const int i = r * cols + c;
    return c < cols && i >= 0 && i < int(cpus_.size()) ? i : -1;
}

bool CpuHeatmap::event(QEvent* e)
{
    if (e->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(e);
        const int i = cellAt(he->pos());
        if (i >= 0) {
            const CpuCoreSample& c = cpus_[size_t(i)];
            QToolTip::showText(he->globalPos(),
                               tr("CPU %1\nbusy %2 (user %3, system %4)\nirq %5 · softirq %6\nfrequency %7\n"
                                  "run-queue wait %8 (avg tasks waiting ×100)\n%9 interrupts/s · %10 softirqs/s\n\n"
                                  "Fill: utilisation · number: GHz · red corner: run-queue contention · amber bar: IRQ time")
                                   .arg(c.cpu)
                                   .arg(fmt::percent(c.load.busy), fmt::percent(c.load.user), fmt::percent(c.load.system),
                                        fmt::percent(c.load.irq), fmt::percent(c.load.softirq), fmt::mhz(c.freqMHz),
                                        fmt::percent(c.runDelayPct), fmt::count(c.irqPs), fmt::count(c.softirqPs)),
                               this);
        } else {
            QToolTip::hideText();
        }
        return true;
    }
    return QWidget::event(e);
}

void CpuHeatmap::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    const int cols = columns(width());
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    QFont bold = font();
    bold.setBold(true);

    for (size_t i = 0; i < cpus_.size(); ++i) {
        const CpuCoreSample& c = cpus_[i];
        const QRectF r((int(i) % cols) * (kCellW + kGap), (int(i) / cols) * (kCellH + kGap), kCellW, kCellH);
        QColor fill = c.online ? theme::heat(c.load.busy / 100.0) : pal.color(QPalette::Mid);
        fill.setAlpha(c.online ? int(50 + c.load.busy * 1.8) : 60);
        p.setPen(QPen(theme::gridLine(pal), 1));
        p.setBrush(fill);
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);

        // IRQ share bar along the bottom.
        const double irq = std::min(100.f, c.load.irq + c.load.softirq);
        if (irq >= 1) {
            p.setPen(Qt::NoPen);
            p.setBrush(theme::amber());
            p.drawRect(QRectF(r.left() + 3, r.bottom() - 4, (r.width() - 6) * irq / 100.0, 2.5));
        }
        // Run-queue contention marker.
        if (c.runDelayPct >= 5) {
            p.setPen(Qt::NoPen);
            p.setBrush(theme::red());
            const double s = std::min(14.0, 5 + c.runDelayPct / 10.0);
            QPolygonF tri;
            tri << QPointF(r.right() - s, r.top()) << r.topRight() << QPointF(r.right(), r.top() + s);
            p.drawPolygon(tri);
        }

        p.setPen(pal.color(QPalette::Text));
        p.setFont(small);
        p.drawText(r.adjusted(5, 2, -4, 0), Qt::AlignLeft | Qt::AlignTop, QStringLiteral("CPU %1").arg(c.cpu));
        p.drawText(r.adjusted(5, 0, -5, -5), Qt::AlignRight | Qt::AlignBottom, fmt::percent(c.load.busy, 0));
        p.setFont(bold);
        p.drawText(r.adjusted(5, 0, -4, -5), Qt::AlignLeft | Qt::AlignBottom,
                   c.freqMHz > 0 ? QString::number(c.freqMHz / 1000.0, 'f', 2) : QStringLiteral("–"));
    }
}

} // namespace culprit
