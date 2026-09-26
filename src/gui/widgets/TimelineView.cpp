// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/widgets/TimelineView.h"

#include "gui/Theme.h"

#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace culprit {

namespace {

double niceStep(double range, int targetTicks)
{
    const double raw = range / std::max(1, targetTicks);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (raw <= m * mag)
            return m * mag;
    return 10 * mag;
}

QString fmtNum(double v)
{
    if (std::fabs(v) >= 100 || v == std::floor(v))
        return QString::number(v, 'f', 0);
    return QString::number(v, 'g', 3);
}

} // namespace

TimelineView::TimelineView(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void TimelineView::setSeries(const std::vector<Series>& s)
{
    series_ = s;
    update();
}

void TimelineView::setPoints(int index, std::vector<QPointF> pts)
{
    if (index >= 0 && index < int(series_.size()))
        series_[size_t(index)].pts = std::move(pts);
    update();
}

void TimelineView::setMarkers(std::vector<Marker> m)
{
    markers_ = std::move(m);
    update();
}

void TimelineView::setXRange(double min, double max)
{
    xMin_ = min;
    xMax_ = std::max(max, min + 1e-6);
    update();
}

void TimelineView::setYRange(double min, double max, bool autoMax)
{
    yMin_ = min;
    yMax_ = max;
    yAutoFloor_ = max;
    yAuto_ = autoMax;
    update();
}

void TimelineView::setRightYRange(double min, double max, bool autoMax)
{
    ryMin_ = min;
    ryMax_ = max;
    ryAutoFloor_ = max;
    ryAuto_ = autoMax;
    update();
}

void TimelineView::setThreshold(double y, const QString& label)
{
    threshold_ = y;
    thresholdLabel_ = label;
    update();
}

void TimelineView::setWallClockBase(qint64 wallMsAtZero)
{
    wallBase_ = wallMsAtZero;
    update();
}

void TimelineView::setSelectedMarker(qint64 id)
{
    selected_ = id;
    update();
}

QRectF TimelineView::plotRect() const
{
    const bool right = std::any_of(series_.begin(), series_.end(), [](const Series& s) { return s.rightAxis; });
    const QFontMetrics fm(font());
    const double left = fm.horizontalAdvance(QStringLiteral("0000 ms")) + 10;
    const double rightPad = right ? fm.horizontalAdvance(QStringLiteral("0000 °C")) + 10 : 12;
    return QRectF(left, 22, width() - left - rightPad, height() - 22 - fm.height() - 10);
}

double TimelineView::xToPx(double x, const QRectF& r) const { return r.left() + (x - xMin_) / (xMax_ - xMin_) * r.width(); }

double TimelineView::pxToX(double px, const QRectF& r) const { return xMin_ + (px - r.left()) / r.width() * (xMax_ - xMin_); }

QString TimelineView::xLabel(double x) const
{
    if (wallBase_ >= 0) {
        const QDateTime t = QDateTime::fromMSecsSinceEpoch(wallBase_ + qint64(x * 1000));
        return t.toString((xMax_ - xMin_) < 120 ? QStringLiteral("HH:mm:ss") : QStringLiteral("HH:mm"));
    }
    if (std::fabs(x) < 1e-9)
        return tr("now");
    return QStringLiteral("%1 s").arg(fmtNum(x));
}

void TimelineView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const QPalette pal = palette();
    p.fillRect(rect(), theme::cardBackground(pal));
    const QRectF r = plotRect();
    if (r.width() < 20 || r.height() < 20)
        return;

    // Y range
    double yMax = yMax_, ryMax = ryMax_;
    if (yAuto_ || ryAuto_) {
        double m = yAutoFloor_, rm = ryAutoFloor_;
        for (const Series& s : series_)
            for (const QPointF& pt : s.pts)
                if (pt.x() >= xMin_ && pt.x() <= xMax_ && std::isfinite(pt.y()))
                    (s.rightAxis ? rm : m) = std::max(s.rightAxis ? rm : m, pt.y() * 1.08);
        if (yAuto_)
            yMax = m;
        if (ryAuto_)
            ryMax = rm;
    }
    auto yPx = [&](double y, bool right) {
        const double lo = right ? ryMin_ : yMin_, hi = right ? ryMax : yMax;
        return r.bottom() - (std::clamp(y, lo, hi) - lo) / (hi - lo) * r.height();
    };

    // Grid + axes labels
    const QFontMetrics fm(font());
    p.setPen(QPen(theme::gridLine(pal), 1));
    const double ys = niceStep(yMax - yMin_, 4);
    for (double y = std::ceil(yMin_ / ys) * ys; y <= yMax + 1e-9; y += ys) {
        const double py = yPx(y, false);
        p.setPen(QPen(theme::gridLine(pal), 1));
        p.drawLine(QPointF(r.left(), py), QPointF(r.right(), py));
        p.setPen(theme::dimText(pal));
        p.drawText(QRectF(0, py - fm.height() / 2.0, r.left() - 6, fm.height()), Qt::AlignRight | Qt::AlignVCenter,
                   fmtNum(y) + (yUnit_.isEmpty() ? QString() : QLatin1Char(' ') + yUnit_));
    }
    if (std::any_of(series_.begin(), series_.end(), [](const Series& s) { return s.rightAxis; })) {
        const double rs = niceStep(ryMax - ryMin_, 4);
        p.setPen(theme::dimText(pal));
        for (double y = std::ceil(ryMin_ / rs) * rs; y <= ryMax + 1e-9; y += rs)
            p.drawText(QRectF(r.right() + 6, yPx(y, true) - fm.height() / 2.0, width() - r.right() - 6, fm.height()),
                       Qt::AlignLeft | Qt::AlignVCenter, fmtNum(y) + QLatin1Char(' ') + rightUnit_);
    }
    const double xs = niceStep(xMax_ - xMin_, std::max(2, int(r.width() / 110)));
    for (double x = std::ceil(xMin_ / xs) * xs; x <= xMax_ + 1e-9; x += xs) {
        const double px = xToPx(x, r);
        p.setPen(QPen(theme::gridLine(pal), 1));
        p.drawLine(QPointF(px, r.top()), QPointF(px, r.bottom()));
        p.setPen(theme::dimText(pal));
        p.drawText(QRectF(px - 60, r.bottom() + 4, 120, fm.height()), Qt::AlignHCenter | Qt::AlignTop, xLabel(x));
    }
    p.setPen(QPen(theme::gridLine(pal).darker(150), 1));
    p.drawLine(r.bottomLeft(), r.bottomRight());

    // Markers (under the data)
    for (const Marker& m : markers_) {
        if (m.x < xMin_ || m.x > xMax_)
            continue;
        QColor c = m.color;
        c.setAlpha(m.id == selected_ ? 230 : 120);
        p.setPen(QPen(c, m.id == selected_ ? 3 : 1.5));
        const double px = xToPx(m.x, r);
        p.drawLine(QPointF(px, r.top()), QPointF(px, r.bottom()));
        p.setBrush(c);
        p.setPen(Qt::NoPen);
        QPolygonF tri;
        tri << QPointF(px - 4, r.top() - 7) << QPointF(px + 4, r.top() - 7) << QPointF(px, r.top());
        p.drawPolygon(tri);
    }

    // Threshold
    if (threshold_ > yMin_ && threshold_ < yMax) {
        QColor c = theme::red();
        c.setAlpha(170);
        p.setPen(QPen(c, 1, Qt::DashLine));
        const double py = yPx(threshold_, false);
        p.drawLine(QPointF(r.left(), py), QPointF(r.right(), py));
        p.drawText(QPointF(r.right() - fm.horizontalAdvance(thresholdLabel_) - 4, py - 3), thresholdLabel_);
    }

    p.setRenderHint(QPainter::Antialiasing, false);
    // Series. Sparse data is drawn point to point; dense data is reduced to
    // min/max per pixel column so spikes survive. No antialiasing: this is
    // repainted continuously and spiky data looks the same without it.
    bool anyData = false;
    const int cols = std::max(1, int(r.width()));
    for (const Series& s : series_) {
        auto first = std::lower_bound(s.pts.begin(), s.pts.end(), xMin_, [](const QPointF& p, double v) { return p.x() < v; });
        auto last = std::upper_bound(first, s.pts.end(), xMax_, [](double v, const QPointF& p) { return v < p.x(); });
        const auto n = last - first;
        if (n <= 0)
            continue;
        anyData = true;
        QPolygonF line;
        std::vector<QLineF> whiskers;   // min..max range of a reduced column
        if (n <= cols) {
            line.reserve(int(n));
            for (auto it = first; it != last; ++it)
                if (std::isfinite(it->y()))
                    line << QPointF(xToPx(it->x(), r), yPx(it->y(), s.rightAxis));
        } else {
            std::vector<double> lo(size_t(cols), NAN), hi(size_t(cols), NAN);
            for (auto it = first; it != last; ++it) {
                if (!std::isfinite(it->y()))
                    continue;
                const size_t c = size_t(std::clamp(int(xToPx(it->x(), r) - r.left()), 0, cols - 1));
                lo[c] = std::isnan(lo[c]) ? it->y() : std::min(lo[c], it->y());
                hi[c] = std::isnan(hi[c]) ? it->y() : std::max(hi[c], it->y());
            }
            for (int c = 0; c < cols; ++c) {
                if (std::isnan(hi[size_t(c)]))
                    continue;
                const double x = r.left() + c;
                line << QPointF(x, yPx(hi[size_t(c)], s.rightAxis));
                if (hi[size_t(c)] > lo[size_t(c)])
                    whiskers.emplace_back(x, yPx(lo[size_t(c)], s.rightAxis), x, yPx(hi[size_t(c)], s.rightAxis));
            }
        }
        if (line.isEmpty())
            continue;
        if (s.fill) {
            // Filled area drawn as vertical bars: scan-converting a polygon with
            // thousands of spiky vertices is by far the most expensive paint op.
            // (rectangle fills hit the rasterizer's fastest path).
            const double barW = line.size() > 1 ? std::max(1.0, std::ceil((line.last().x() - line.first().x()) / (line.size() - 1))) : 1.0;
            QColor fc = s.color;
            fc.setAlpha(70);
            for (const QPointF& pt : line)
                if (pt.y() < r.bottom() - 0.5)
                    p.fillRect(QRectF(pt.x() - barW / 2, pt.y(), barW, r.bottom() - pt.y()), fc);
        }
        QPen linePen(s.color, 1);
        linePen.setCosmetic(true);   // 1 px cosmetic pens skip the (slow) stroker
        p.setPen(linePen);
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(line);
        if (!whiskers.empty()) {
            QColor wc = s.color;
            wc.setAlpha(120);
            QPen wp(wc, 1);
            wp.setCosmetic(true);
            p.setPen(wp);
            p.drawLines(whiskers.data(), int(whiskers.size()));
        }
    }

    // Legend
    double lx = r.left();
    for (const Series& s : series_) {
        if (s.name.isEmpty())
            continue;
        p.setPen(Qt::NoPen);
        p.setBrush(s.color);
        p.drawRect(QRectF(lx, 7, 10, 10));
        p.setPen(pal.color(QPalette::Text));
        p.drawText(QPointF(lx + 14, 7 + fm.ascent() - 2), s.name);
        lx += 26 + fm.horizontalAdvance(s.name);
    }

    if (!anyData && !empty_.isEmpty()) {
        p.setPen(theme::dimText(pal));
        p.drawText(r, Qt::AlignCenter | Qt::TextWordWrap, empty_);
    }

    // Hover cursor
    if (hoverX_ >= r.left() && hoverX_ <= r.right()) {
        p.setPen(QPen(theme::dimText(pal), 1, Qt::DotLine));
        p.drawLine(QPointF(hoverX_, r.top()), QPointF(hoverX_, r.bottom()));
    }
}

int TimelineView::markerAt(const QPoint& pos) const
{
    const QRectF r = plotRect();
    int best = -1;
    double bestD = 6;
    for (size_t i = 0; i < markers_.size(); ++i) {
        const double d = std::fabs(xToPx(markers_[i].x, r) - pos.x());
        if (d < bestD) {
            bestD = d;
            best = int(i);
        }
    }
    return best;
}

void TimelineView::mouseMoveEvent(QMouseEvent* e)
{
    hoverX_ = int(e->position().x());
    const QRectF r = plotRect();
    const int m = markerAt(e->position().toPoint());
    if (m >= 0) {
        QToolTip::showText(e->globalPosition().toPoint(), markers_[size_t(m)].tooltip, this);
    } else if (r.contains(e->position())) {
        const double x = pxToX(e->position().x(), r);
        QString tip = xLabel(x);
        for (const Series& s : series_) {
            // nearest point
            auto it = std::lower_bound(s.pts.begin(), s.pts.end(), x, [](const QPointF& p, double v) { return p.x() < v; });
            if (it == s.pts.end() && !s.pts.empty())
                --it;
            if (it != s.pts.end())
                tip += QStringLiteral("\n%1: %2").arg(s.name.isEmpty() ? tr("value") : s.name, fmtNum(it->y()));
        }
        QToolTip::showText(e->globalPosition().toPoint(), tip, this);
    }
    update();
}

void TimelineView::mousePressEvent(QMouseEvent* e)
{
    const int m = markerAt(e->position().toPoint());
    if (m >= 0) {
        selected_ = markers_[size_t(m)].id;
        emit markerClicked(selected_);
        update();
    }
}

void TimelineView::leaveEvent(QEvent*)
{
    hoverX_ = -1;
    update();
}

} // namespace culprit
