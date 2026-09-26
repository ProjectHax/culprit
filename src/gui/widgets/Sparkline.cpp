// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/widgets/Sparkline.h"

#include "gui/Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <cmath>
#include <limits>

namespace culprit {

Sparkline::Sparkline(const QString& title, QWidget* parent)
    : QWidget(parent), title_(title), threshold_(std::numeric_limits<double>::quiet_NaN())
{
    setSeries({QString()});
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void Sparkline::setSeries(const QStringList& names)
{
    names_ = names;
    data_.assign(size_t(names.size()), {});
    update();
}

void Sparkline::setFixedRange(double min, double max)
{
    fixed_ = true;
    min_ = min;
    max_ = max;
}

void Sparkline::setAutoRange(double minimumMax)
{
    fixed_ = false;
    min_ = 0;
    minimumMax_ = minimumMax;
}

void Sparkline::setThreshold(double v)
{
    threshold_ = v;
    update();
}

void Sparkline::setCapacity(int points) { capacity_ = std::max(2, points); }

void Sparkline::push(const std::vector<double>& values)
{
    for (size_t i = 0; i < data_.size(); ++i) {
        auto& d = data_[i];
        d.push_back(i < values.size() ? values[i] : std::numeric_limits<double>::quiet_NaN());
        while (int(d.size()) > capacity_)
            d.pop_front();
    }
    update();
}

void Sparkline::setValueText(const QString& text, bool alert)
{
    value_ = text;
    alert_ = alert;
    update();
}

void Sparkline::setSubText(const QString& text)
{
    sub_ = text;
    update();
}

void Sparkline::clear()
{
    for (auto& d : data_)
        d.clear();
    update();
}

void Sparkline::mousePressEvent(QMouseEvent* e)
{
    pressed_ = e->button() == Qt::LeftButton;
}

void Sparkline::mouseReleaseEvent(QMouseEvent* e)
{
    // A click is press + release on this card (not a release from a drag elsewhere).
    if (pressed_ && e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()))
        emit clicked();
    pressed_ = false;
}

void Sparkline::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    const QRectF card = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);

    p.setPen(QPen(theme::gridLine(pal), 1));
    p.setBrush(theme::cardBackground(pal));
    p.drawRoundedRect(card, 8, 8);

    // Header
    QFont titleFont = font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 0.92);
    QFont valueFont = font();
    valueFont.setBold(true);
    valueFont.setPointSizeF(valueFont.pointSizeF() * 1.15);
    const int pad = 10;
    QRectF header = card.adjusted(pad, 6, -pad, 0);
    p.setFont(titleFont);
    p.setPen(theme::dimText(pal));
    const QFontMetricsF tfm(titleFont);
    p.drawText(QRectF(header.left(), header.top(), header.width() * 0.55, tfm.height()),
               Qt::AlignLeft | Qt::AlignVCenter, tfm.elidedText(title_, Qt::ElideRight, header.width() * 0.55));
    p.setFont(valueFont);
    p.setPen(alert_ ? theme::red() : pal.color(QPalette::Text));
    const QFontMetricsF vfm(valueFont);
    p.drawText(QRectF(header.left() + header.width() * 0.3, header.top() - 1, header.width() * 0.7, vfm.height()),
               Qt::AlignRight | Qt::AlignVCenter, value_);

    double top = header.top() + std::max(tfm.height(), vfm.height()) + 1;
    if (!sub_.isEmpty()) {
        p.setFont(titleFont);
        p.setPen(theme::dimText(pal));
        p.drawText(QRectF(header.left(), top, header.width(), tfm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   tfm.elidedText(sub_, Qt::ElideRight, header.width()));
        top += tfm.height();
    }

    QRectF plot(card.left() + pad, top + 4, card.width() - 2 * pad, card.bottom() - top - 10);
    if (plot.height() < 8)
        return;

    // Range
    double lo = min_, hi = max_;
    if (!fixed_) {
        hi = minimumMax_;
        for (const auto& d : data_)
            for (double v : d)
                if (std::isfinite(v))
                    hi = std::max(hi, v * 1.1);
    }
    if (hi <= lo)
        hi = lo + 1;

    p.setPen(QPen(theme::gridLine(pal), 1));
    p.drawLine(plot.bottomLeft(), plot.bottomRight());

    auto yOf = [&](double v) { return plot.bottom() - (std::clamp(v, lo, hi) - lo) / (hi - lo) * plot.height(); };
    const double step = plot.width() / double(capacity_ - 1);

    if (std::isfinite(threshold_) && threshold_ > lo && threshold_ < hi) {
        QPen tp(theme::red(), 1, Qt::DashLine);
        tp.setColor(QColor(theme::red().red(), theme::red().green(), theme::red().blue(), 150));
        p.setPen(tp);
        const double y = yOf(threshold_);
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }

    for (size_t s = 0; s < data_.size(); ++s) {
        const auto& d = data_[s];
        if (d.size() < 2)
            continue;
        const QColor c = theme::series(int(s));
        QPainterPath line;
        bool pen = false;
        const double x0 = plot.right() - double(d.size() - 1) * step;
        double firstX = 0, lastX = 0;
        for (size_t i = 0; i < d.size(); ++i) {
            const double v = d[i];
            const double x = x0 + double(i) * step;
            if (!std::isfinite(v)) {
                pen = false;
                continue;
            }
            if (!pen) {
                line.moveTo(x, yOf(v));
                if (line.elementCount() == 1)
                    firstX = x;
                pen = true;
            } else {
                line.lineTo(x, yOf(v));
            }
            lastX = x;
        }
        if (s == 0 && !line.isEmpty()) {
            QPainterPath fill = line;
            fill.lineTo(lastX, plot.bottom());
            fill.lineTo(firstX, plot.bottom());
            fill.closeSubpath();
            QColor fc = c;
            fc.setAlpha(45);
            p.fillPath(fill, fc);
        }
        p.setPen(QPen(c, 1.6));
        p.setBrush(Qt::NoBrush);
        p.drawPath(line);
    }

    // Legend for multi-series cards
    if (data_.size() > 1) {
        p.setFont(titleFont);
        double x = plot.left() + 2;
        for (int s = 0; s < names_.size(); ++s) {
            if (names_[s].isEmpty())
                continue;
            p.setPen(Qt::NoPen);
            p.setBrush(theme::series(s));
            p.drawEllipse(QPointF(x + 3, plot.top() + 6), 3, 3);
            p.setPen(theme::dimText(pal));
            p.drawText(QPointF(x + 9, plot.top() + 6 + tfm.ascent() / 2 - 1), names_[s]);
            x += 16 + tfm.horizontalAdvance(names_[s]);
        }
    }
}

} // namespace culprit
