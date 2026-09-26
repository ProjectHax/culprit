// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/widgets/TitleBar.h"

#include "gui/ThemeManager.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QWindow>

namespace culprit {

// ------------------------------------------------------------------ WindowButton

WindowButton::WindowButton(Kind kind, QWidget* parent) : QAbstractButton(parent), kind_(kind)
{
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::ArrowCursor);
}

void WindowButton::setKind(Kind k)
{
    kind_ = k;
    update();
}

void WindowButton::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ThemeTokens& t = ThemeManager::instance().tokens();
    const bool hot = underMouse();
    const bool down = isDown();

    // Round hover disc (red for close), like modern GNOME/Windows buttons.
    const double d = std::min(width(), height()) - 4.0;
    const QRectF disc((width() - d) / 2.0, (height() - d) / 2.0, d, d);
    if (hot || down) {
        QColor bg = kind_ == Close ? QColor(0xe5, 0x48, 0x4d) : (down ? t.pressed : t.hover);
        if (kind_ == Close && down)
            bg = bg.darker(115);
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawEllipse(disc);
    }
    QColor fg = kind_ == Close && (hot || down) ? QColor(Qt::white) : t.text;
    if (!window()->isActiveWindow() && !hot)
        fg = t.textDim;
    QPen pen(fg, 1.25);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const QPointF c = disc.center();
    const double g = d * 0.18;   // glyph half-size
    switch (kind_) {
    case Minimize:
        p.drawLine(QPointF(c.x() - g, c.y() + 0.5), QPointF(c.x() + g, c.y() + 0.5));
        break;
    case Maximize:
        p.drawRoundedRect(QRectF(c.x() - g, c.y() - g, 2 * g, 2 * g), 1.5, 1.5);
        break;
    case Restore:
        p.drawRoundedRect(QRectF(c.x() - g, c.y() - g + 2, 2 * g - 2, 2 * g - 2), 1.2, 1.2);
        p.drawPolyline(QPolygonF{QPointF(c.x() - g + 2, c.y() - g), QPointF(c.x() + g, c.y() - g), QPointF(c.x() + g, c.y() + g - 2)});
        break;
    case Close:
        p.drawLine(QPointF(c.x() - g, c.y() - g), QPointF(c.x() + g, c.y() + g));
        p.drawLine(QPointF(c.x() - g, c.y() + g), QPointF(c.x() + g, c.y() - g));
        break;
    }
}

// ------------------------------------------------------------------ TitleBar

namespace {
// 1px vertical divider that follows the theme.
class Separator : public QWidget {
public:
    Separator() { setFixedSize(1, 18); }

protected:
    void paintEvent(QPaintEvent*) override { QPainter(this).fillRect(rect(), ThemeManager::instance().tokens().border); }
};
} // namespace

TitleBar::TitleBar(QWidget* window, int buttons, QWidget* parent) : QWidget(parent), window_(window), buttons_(buttons)
{
    setAttribute(Qt::WA_StyledBackground, false);
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(10, 4, 6, 4);
    h->setSpacing(4);

    icon_ = new QLabel;
    icon_->setFixedSize(20, 20);
    title_ = new QLabel;
    QFont f = title_->font();
    f.setWeight(QFont::DemiBold);
    title_->setFont(f);
    h->addWidget(icon_);
    h->addSpacing(4);
    h->addWidget(title_);
    h->addSpacing(14);
    actions_ = new QHBoxLayout;
    actions_->setSpacing(2);
    h->addLayout(actions_);
    h->addStretch(1);

    if (buttons_ & Minimize) {
        min_ = new WindowButton(WindowButton::Minimize);
        min_->setToolTip(tr("Minimize"));
        connect(min_, &QAbstractButton::clicked, window_, &QWidget::showMinimized);
        h->addWidget(min_);
    }
    if (buttons_ & Maximize) {
        max_ = new WindowButton(WindowButton::Maximize);
        connect(max_, &QAbstractButton::clicked, this, &TitleBar::toggleMaximized);
        h->addWidget(max_);
    }
    if (buttons_ & Close) {
        close_ = new WindowButton(WindowButton::Close);
        close_->setToolTip(tr("Close"));
        connect(close_, &QAbstractButton::clicked, window_, &QWidget::close);
        h->addWidget(close_);
    }
    window_->installEventFilter(this);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { syncState(); });
    syncState();
}

QSize TitleBar::sizeHint() const
{
    QSize s = QWidget::sizeHint();
    s.setHeight(std::max(s.height(), 40));
    return s;
}

void TitleBar::addAction(QAction* action)
{
    auto* b = new QToolButton;
    b->setDefaultAction(action);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    b->setFocusPolicy(Qt::NoFocus);
    actions_->addWidget(b);
}

void TitleBar::addSeparator()
{
    actions_->addSpacing(4);
    actions_->addWidget(new Separator);
    actions_->addSpacing(4);
}

void TitleBar::syncState()
{
    title_->setText(window_->windowTitle());
    const QIcon icon = window_->windowIcon().isNull() ? QApplication::windowIcon() : window_->windowIcon();
    icon_->setPixmap(icon.pixmap(QSize(20, 20), devicePixelRatioF()));
    if (max_) {
        const bool maximized = window_->windowState() & Qt::WindowMaximized;
        max_->setKind(maximized ? WindowButton::Restore : WindowButton::Maximize);
        max_->setToolTip(maximized ? tr("Restore") : tr("Maximize"));
    }
    QPalette pal = title_->palette();
    const ThemeTokens& t = ThemeManager::instance().tokens();
    pal.setColor(QPalette::WindowText, window_->isActiveWindow() ? t.text : t.textDim);
    title_->setPalette(pal);
    update();
}

bool TitleBar::eventFilter(QObject* o, QEvent* e)
{
    if (o == window_) {
        switch (e->type()) {
        case QEvent::WindowStateChange:
        case QEvent::WindowTitleChange:
        case QEvent::WindowIconChange:
        case QEvent::ActivationChange:
            syncState();
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(o, e);
}

void TitleBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const ThemeTokens& t = ThemeManager::instance().tokens();
    p.fillRect(rect(), window_->isActiveWindow() ? t.titleBar : t.titleBarInactive);
    p.setPen(t.border);
    p.drawLine(QPointF(0, height() - 0.5), QPointF(width(), height() - 0.5));
}

void TitleBar::toggleMaximized()
{
    if (window_->windowState() & Qt::WindowMaximized)
        window_->showNormal();
    else
        window_->showMaximized();
}

void TitleBar::mousePressEvent(QMouseEvent* e)
{
    // Empty title-bar space moves the window (the compositor handles snapping).
    if (e->button() == Qt::LeftButton && window_->windowHandle()) {
        window_->windowHandle()->startSystemMove();
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && max_) {
        toggleMaximized();
        e->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(e);
}

void TitleBar::contextMenuEvent(QContextMenuEvent* e)
{
    QMenu menu(this);
    if (min_)
        menu.addAction(tr("Minimize"), window_, &QWidget::showMinimized);
    if (max_)
        menu.addAction(window_->windowState() & Qt::WindowMaximized ? tr("Restore") : tr("Maximize"), this, &TitleBar::toggleMaximized);
    menu.addSeparator();
    menu.addAction(tr("Close"), window_, &QWidget::close);
    menu.exec(e->globalPos());
}

} // namespace culprit
