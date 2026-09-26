// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/widgets/WindowChrome.h"

#include "gui/ThemeManager.h"
#include "gui/widgets/TitleBar.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <QWindow>

#include <optional>

#ifdef CULPRIT_HAVE_WAYLAND_CLIENT
#include <wayland-client.h>
#include <cstring>
#endif

namespace culprit {

namespace {

constexpr int kEdge = 6;      // resize band, logical px
constexpr int kCorner = 14;   // larger grab area at the corners

bool gIntegrated = false;

// Does the Wayland compositor offer server-side decorations (xdg-decoration)?
// Asked once on a separate, short-lived connection so Qt's own is untouched.
std::optional<bool> compositorDrawsDecorations()
{
#ifdef CULPRIT_HAVE_WAYLAND_CLIENT
    static std::optional<bool> cached;
    static bool asked = false;
    if (asked)
        return cached;
    asked = true;
    wl_display* d = wl_display_connect(nullptr);
    if (!d)
        return cached;
    bool ssd = false;
    static const wl_registry_listener listener = {
        [](void* data, wl_registry*, uint32_t, const char* iface, uint32_t) {
            if (!std::strcmp(iface, "zxdg_decoration_manager_v1") || !std::strcmp(iface, "org_kde_kwin_server_decoration_manager"))
                *static_cast<bool*>(data) = true;
        },
        [](void*, wl_registry*, uint32_t) {}};
    wl_registry* reg = wl_display_get_registry(d);
    wl_registry_add_listener(reg, &listener, &ssd);
    if (wl_display_roundtrip(d) >= 0)
        cached = ssd;
    wl_registry_destroy(reg);
    wl_display_disconnect(d);
    return cached;
#else
    return std::nullopt;
#endif
}

} // namespace

TitleBarDecision decideTitleBar(TitleBarMode mode)
{
    if (mode == TitleBarMode::Integrated)
        return {true, QObject::tr("Culprit's own title bar (chosen in settings)")};
    if (mode == TitleBarMode::System)
        return {false, QObject::tr("the system's title bar (chosen in settings)")};

    const QString platform = QGuiApplication::platformName();
    if (!platform.startsWith(QLatin1String("wayland")))
        return {false, QObject::tr("the window manager's title bar (%1 session)").arg(platform)};
    if (qEnvironmentVariableIntValue("QT_WAYLAND_DISABLE_WINDOWDECORATION") > 0)
        return {true, QObject::tr("Culprit's own title bar (Qt window decorations are disabled)")};
    std::optional<bool> ssd = compositorDrawsDecorations();
    if (!ssd) {
        // No way to ask: GNOME is the common compositor without server-side decorations.
        ssd = !qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains(QLatin1String("GNOME"), Qt::CaseInsensitive);
    }
    if (*ssd)
        return {false, QObject::tr("the compositor's title bar")};
    return {true, QObject::tr("Culprit's own title bar — this compositor doesn't draw title bars for Qt apps, and Qt's "
                              "fallback ignores QT_SCALE_FACTOR")};
}

// ------------------------------------------------------------------ WindowChrome

bool WindowChrome::integratedTitleBars() { return gIntegrated; }
void WindowChrome::setIntegratedTitleBars(bool on) { gIntegrated = on; }

WindowChrome::WindowChrome(QWidget* window) : QObject(window), window_(window)
{
    window_->installEventFilter(this);
}

void WindowChrome::setActive(bool active)
{
    active_ = active;
    if (!active)
        setEdgeCursor({});
    attach();
    window_->update();
}

void WindowChrome::attach()
{
    // The native window is recreated when window flags change: follow it.
    QWindow* h = window_->windowHandle();
    if (h == handle_)
        return;
    if (handle_)
        handle_->removeEventFilter(this);
    handle_ = h;
    if (handle_)
        handle_->installEventFilter(this);
}

Qt::Edges WindowChrome::edgesAt(const QPointF& pos) const
{
    if (!handle_)
        return {};
    const double w = handle_->width(), h = handle_->height();
    const double x = pos.x(), y = pos.y();
    Qt::Edges e;
    const bool nearLeft = x < kEdge, nearRight = x >= w - kEdge, nearTop = y < kEdge, nearBottom = y >= h - kEdge;
    if (nearLeft || (x < kCorner && (nearTop || nearBottom)))
        e |= Qt::LeftEdge;
    if (nearRight || (x >= w - kCorner && (nearTop || nearBottom)))
        e |= Qt::RightEdge;
    if (nearTop || (y < kCorner && (nearLeft || nearRight)))
        e |= Qt::TopEdge;
    if (nearBottom || (y >= h - kCorner && (nearLeft || nearRight)))
        e |= Qt::BottomEdge;
    return e;
}

void WindowChrome::setEdgeCursor(Qt::Edges edges)
{
    if (edges == cursorEdges_)
        return;
    cursorEdges_ = edges;
    Qt::CursorShape shape = Qt::ArrowCursor;
    if (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge))
        shape = Qt::SizeFDiagCursor;
    else if (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge))
        shape = Qt::SizeBDiagCursor;
    else if (edges & (Qt::LeftEdge | Qt::RightEdge))
        shape = Qt::SizeHorCursor;
    else if (edges & (Qt::TopEdge | Qt::BottomEdge))
        shape = Qt::SizeVerCursor;
    // An override cursor wins over whatever child widget is under the mouse.
    if (edges) {
        if (overrideCursor_)
            QGuiApplication::changeOverrideCursor(shape);
        else
            QGuiApplication::setOverrideCursor(shape);
        overrideCursor_ = true;
    } else if (overrideCursor_) {
        QGuiApplication::restoreOverrideCursor();
        overrideCursor_ = false;
    }
}

bool WindowChrome::eventFilter(QObject* o, QEvent* e)
{
    if (o == window_) {
        if (e->type() == QEvent::Show || e->type() == QEvent::WinIdChange)
            attach();
        return false;
    }
    if (o != handle_ || !active_)
        return false;
    const bool resizable = !(window_->windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen)) &&
                           window_->minimumSize() != window_->maximumSize();
    switch (e->type()) {
    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(e);
        if (me->buttons() == Qt::NoButton)
            setEdgeCursor(resizable ? edgesAt(me->position()) : Qt::Edges{});
        break;
    }
    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(e);
        if (resizable && me->button() == Qt::LeftButton) {
            const Qt::Edges edges = edgesAt(me->position());
            if (edges && handle_->startSystemResize(edges)) {
                setEdgeCursor({});
                return true;   // the compositor owns this drag now
            }
        }
        break;
    }
    case QEvent::Leave:
        setEdgeCursor({});
        break;
    default:
        break;
    }
    return false;
}

void WindowChrome::paintOutline(QWidget* w)
{
    if (w->windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))
        return;
    QPainter p(w);
    const ThemeTokens& t = ThemeManager::instance().tokens();
    p.setPen(QPen(t.dark ? t.borderStrong : t.borderStrong.darker(108), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(w->rect()).adjusted(0.5, 0.5, -0.5, -0.5));
}

// ------------------------------------------------------------------ ChromeDialog

ChromeDialog::ChromeDialog(QWidget* parent, const QString& title) : QDialog(parent)
{
    setWindowTitle(title);
    integrated_ = WindowChrome::integratedTitleBars();
    auto* outer = new QVBoxLayout(this);
    outer->setSpacing(0);
    auto* content = new QWidget;
    body_ = new QVBoxLayout(content);
    body_->setContentsMargins(16, 14, 16, 14);
    body_->setSpacing(10);
    if (integrated_) {
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        outer->setContentsMargins(1, 1, 1, 1);   // room for the outline
        outer->addWidget(new TitleBar(this, TitleBar::Close));
        chrome_ = new WindowChrome(this);
        chrome_->setActive(true);
    } else {
        outer->setContentsMargins(0, 0, 0, 0);
    }
    outer->addWidget(content, 1);
}

void ChromeDialog::paintEvent(QPaintEvent* e)
{
    QDialog::paintEvent(e);
    if (integrated_)
        WindowChrome::paintOutline(this);
}

} // namespace culprit
