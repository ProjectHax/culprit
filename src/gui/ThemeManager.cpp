// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/ThemeManager.h"

#include "gui/SystemAppearance.h"

#include <QApplication>
#include <QEvent>
#include <QMenu>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace culprit {

namespace {

QString css(const QColor& c)
{
    return c.alpha() == 255 ? c.name() : QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 3);
}

QColor withAlpha(QColor c, int a)
{
    c.setAlpha(a);
    return c;
}

} // namespace

ThemeManager& ThemeManager::instance()
{
    static ThemeManager* m = new ThemeManager;   // lives for the whole process
    return *m;
}

ThemeManager::ThemeManager() : QObject(qApp)
{
    // Capture what the platform gave us before we replace the palette.
    system_ = new SystemAppearance(QApplication::palette(), this);
    connect(system_, &SystemAppearance::changed, this, [this] {
        if (mode_ == ThemeMode::Auto)
            apply();
    });
    qApp->installEventFilter(this);
}

void ThemeManager::setMode(ThemeMode mode)
{
    mode_ = mode;
    apply();
}

bool ThemeManager::systemPrefersDark() const { return system_->prefersDark(); }

QString ThemeManager::systemDescription() const
{
    return tr("%1, from %2").arg(system_->prefersDark() ? tr("dark") : tr("light"), system_->source());
}

ThemeTokens ThemeManager::lightTokens()
{
    ThemeTokens t;
    t.dark = false;
    t.window = QColor(0xf4, 0xf5, 0xf7);
    t.surface = QColor(0xff, 0xff, 0xff);
    t.surfaceAlt = QColor(0xf7, 0xf8, 0xfa);
    t.card = QColor(0xff, 0xff, 0xff);
    t.border = QColor(0xdd, 0xe0, 0xe5);
    t.borderStrong = QColor(0xc3, 0xc8, 0xd0);
    t.text = QColor(0x1d, 0x21, 0x27);
    t.textDim = QColor(0x5b, 0x63, 0x70);
    t.textFaint = QColor(0x9a, 0xa1, 0xab);
    t.accent = QColor(0x25, 0x63, 0xeb);
    t.accentHover = QColor(0x1d, 0x4f, 0xd8);
    t.accentText = Qt::white;
    t.accentSoft = withAlpha(QColor(0x25, 0x63, 0xeb), 30);
    t.selection = QColor(0xdb, 0xe6, 0xfd);
    t.hover = QColor(0xea, 0xec, 0xf0);
    t.pressed = QColor(0xdf, 0xe2, 0xe8);
    t.titleBar = QColor(0xea, 0xec, 0xef);
    t.titleBarInactive = QColor(0xf1, 0xf2, 0xf4);
    t.tooltipBg = QColor(0x22, 0x26, 0x2c);
    t.tooltipText = QColor(0xf4, 0xf5, 0xf7);
    t.scrollHandle = QColor(0xc6, 0xcb, 0xd3);
    t.scrollHandleHover = QColor(0xa9, 0xb0, 0xbb);
    t.danger = QColor(0xdc, 0x26, 0x26);
    t.warning = QColor(0xd9, 0x77, 0x06);
    t.success = QColor(0x05, 0x96, 0x69);
    return t;
}

ThemeTokens ThemeManager::darkTokens()
{
    ThemeTokens t;
    t.dark = true;
    t.window = QColor(0x1b, 0x1c, 0x20);
    t.surface = QColor(0x22, 0x24, 0x29);
    t.surfaceAlt = QColor(0x26, 0x29, 0x2e);
    t.card = QColor(0x24, 0x26, 0x2b);
    t.border = QColor(0x32, 0x35, 0x3c);
    t.borderStrong = QColor(0x46, 0x4a, 0x53);
    t.text = QColor(0xe6, 0xe8, 0xec);
    t.textDim = QColor(0xa2, 0xa8, 0xb2);
    t.textFaint = QColor(0x6b, 0x71, 0x7b);
    t.accent = QColor(0x5b, 0x96, 0xff);
    t.accentHover = QColor(0x78, 0xa9, 0xff);
    t.accentText = Qt::white;
    t.accentSoft = withAlpha(QColor(0x5b, 0x96, 0xff), 46);
    t.selection = QColor(0x2a, 0x40, 0x66);
    t.hover = QColor(0x2b, 0x2e, 0x34);
    t.pressed = QColor(0x33, 0x37, 0x3e);
    t.titleBar = QColor(0x15, 0x16, 0x19);
    t.titleBarInactive = QColor(0x1b, 0x1c, 0x20);
    t.tooltipBg = QColor(0x31, 0x34, 0x3b);
    t.tooltipText = QColor(0xe6, 0xe8, 0xec);
    t.scrollHandle = QColor(0x41, 0x45, 0x4d);
    t.scrollHandleHover = QColor(0x58, 0x5d, 0x68);
    t.danger = QColor(0xf2, 0x5c, 0x5c);
    t.warning = QColor(0xf5, 0xa5, 0x24);
    t.success = QColor(0x34, 0xd3, 0x99);
    return t;
}

QPalette ThemeManager::paletteFor(const ThemeTokens& t)
{
    QPalette p;
    auto both = [&p](QPalette::ColorRole r, const QColor& c) {
        p.setColor(QPalette::Active, r, c);
        p.setColor(QPalette::Inactive, r, c);
    };
    for (QPalette::ColorGroup g : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        p.setColor(g, QPalette::Window, t.window);
        p.setColor(g, QPalette::Base, t.surface);
        p.setColor(g, QPalette::AlternateBase, t.surfaceAlt);
        p.setColor(g, QPalette::Button, t.surface);
        p.setColor(g, QPalette::ToolTipBase, t.tooltipBg);
        p.setColor(g, QPalette::ToolTipText, t.tooltipText);
        p.setColor(g, QPalette::Light, t.dark ? t.borderStrong : Qt::white);
        p.setColor(g, QPalette::Midlight, t.dark ? t.border : t.surfaceAlt);
        p.setColor(g, QPalette::Mid, t.border);
        p.setColor(g, QPalette::Dark, t.borderStrong);
        p.setColor(g, QPalette::Shadow, t.dark ? QColor(0, 0, 0) : QColor(0x9a, 0xa1, 0xab));
        p.setColor(g, QPalette::Link, t.accent);
        p.setColor(g, QPalette::LinkVisited, t.accentHover);
        p.setColor(g, QPalette::BrightText, t.danger);
        p.setColor(g, QPalette::PlaceholderText, t.textFaint);
        p.setColor(g, QPalette::Accent, t.accent);
    }
    both(QPalette::WindowText, t.text);
    both(QPalette::Text, t.text);
    both(QPalette::ButtonText, t.text);
    both(QPalette::Highlight, t.selection);
    both(QPalette::HighlightedText, t.text);
    p.setColor(QPalette::Disabled, QPalette::WindowText, t.textFaint);
    p.setColor(QPalette::Disabled, QPalette::Text, t.textFaint);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, t.textFaint);
    p.setColor(QPalette::Disabled, QPalette::Highlight, t.hover);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, t.textFaint);
    return p;
}

// Styled on top of Fusion. Deliberately no ::item rules on item views: those
// force per-cell stylesheet rendering and make the big tables slow to paint.
QString ThemeManager::styleSheetFor(const ThemeTokens& t)
{
    const QString mode = t.dark ? QStringLiteral("dark") : QStringLiteral("light");
    QString s = QStringLiteral(R"(
QMainWindow, QDialog { background: @window; }
QToolTip { background: @tooltipBg; color: @tooltipText; border: 1px solid @border; padding: 5px 8px; }

QTabWidget::pane { border: none; border-top: 1px solid @border; top: -1px; }
QTabBar { qproperty-drawBase: 0; }
QTabBar::tab { background: transparent; color: @textDim; padding: 7px 16px; margin-right: 2px;
               border: none; border-bottom: 2px solid transparent; }
QTabBar::tab:hover { color: @text; background: @hover; border-top-left-radius: 6px; border-top-right-radius: 6px; }
QTabBar::tab:selected { color: @text; border-bottom: 2px solid @accent; }

QToolBar { background: @window; border: none; border-bottom: 1px solid @border; spacing: 4px; padding: 4px 6px; }
QToolBar::separator { background: @border; width: 1px; margin: 6px 6px; }
QToolButton { background: transparent; color: @text; border: 1px solid transparent; border-radius: 6px; padding: 4px 10px; }
QToolButton:hover { background: @hover; }
QToolButton:pressed { background: @pressed; }
QToolButton:checked { background: @accentSoft; color: @accent; }

QPushButton { background: @surface; color: @text; border: 1px solid @border; border-radius: 6px; padding: 5px 14px; }
QPushButton:hover { background: @hover; border-color: @borderStrong; }
QPushButton:pressed { background: @pressed; }
QPushButton:checked { background: @accent; color: @accentText; border-color: @accent; }
QPushButton:default { border-color: @accent; }
QPushButton:disabled { color: @textFaint; background: @window; border-color: @border; }
QPushButton#primary { background: @accent; color: @accentText; border-color: @accent; }
QPushButton#primary:hover { background: @accentHover; border-color: @accentHover; }

QLineEdit { background: @surface; color: @text; border: 1px solid @border; border-radius: 6px; padding: 5px 8px;
            selection-background-color: @accent; selection-color: @accentText; }
QLineEdit:hover { border-color: @borderStrong; }
QLineEdit:focus { border-color: @accent; }

QComboBox { background: @surface; color: @text; border: 1px solid @border; border-radius: 6px; padding: 4px 28px 4px 10px; min-height: 20px; }
QComboBox:hover { border-color: @borderStrong; }
QComboBox:focus, QComboBox:on { border-color: @accent; }
QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 24px; border: none; }
QComboBox::down-arrow { image: url(:/theme/@mode/chevron-down.svg); width: 10px; height: 10px; }
QComboBox QAbstractItemView { background: @surface; color: @text; border: 1px solid @border; padding: 4px; outline: none;
                              selection-background-color: @selection; selection-color: @text; }

QAbstractSpinBox { background: @surface; color: @text; border: 1px solid @border; border-radius: 6px; padding: 4px 24px 4px 8px; min-height: 20px; }
QAbstractSpinBox:hover { border-color: @borderStrong; }
QAbstractSpinBox:focus { border-color: @accent; }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { subcontrol-origin: border; width: 20px; border: none; background: transparent; }
QAbstractSpinBox::up-button { subcontrol-position: top right; border-top-right-radius: 6px; }
QAbstractSpinBox::down-button { subcontrol-position: bottom right; border-bottom-right-radius: 6px; }
QAbstractSpinBox::up-button:hover, QAbstractSpinBox::down-button:hover { background: @hover; }
QAbstractSpinBox::up-arrow { image: url(:/theme/@mode/chevron-up.svg); width: 9px; height: 9px; }
QAbstractSpinBox::down-arrow { image: url(:/theme/@mode/chevron-down.svg); width: 9px; height: 9px; }

QCheckBox { color: @text; spacing: 8px; }
QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid @borderStrong; border-radius: 4px; background: @surface; }
QCheckBox::indicator:hover { border-color: @accent; }
QCheckBox::indicator:checked { background: @accent; border-color: @accent; image: url(:/theme/check.svg); }

QGroupBox { background: @card; border: 1px solid @border; border-radius: 8px; margin-top: 16px; padding: 10px 8px 8px 8px; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 10px; top: 2px; padding: 0 4px; color: @textDim; }

QTreeView, QTableView, QListView, QTextBrowser, QTextEdit, QPlainTextEdit {
    background: @surface; alternate-background-color: @surfaceAlt; color: @text; border: 1px solid @border;
    selection-background-color: @selection; selection-color: @text; gridline-color: @border; }
QHeaderView { background: @surface; border: none; }
QHeaderView::section { background: @surface; color: @textDim; border: none; border-bottom: 1px solid @border;
                       border-right: 1px solid @border; padding: 5px 8px; }
QHeaderView::section:hover { background: @hover; color: @text; }
QHeaderView::up-arrow { image: url(:/theme/@mode/chevron-up.svg); width: 9px; height: 9px; subcontrol-position: center right; right: 4px; }
QHeaderView::down-arrow { image: url(:/theme/@mode/chevron-down.svg); width: 9px; height: 9px; subcontrol-position: center right; right: 4px; }
QTableCornerButton::section { background: @surface; border: none; }

QScrollBar:vertical { background: transparent; width: 12px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 12px; margin: 2px; }
QScrollBar::handle:vertical { background: @scrollHandle; border-radius: 4px; min-height: 32px; }
QScrollBar::handle:horizontal { background: @scrollHandle; border-radius: 4px; min-width: 32px; }
QScrollBar::handle:hover { background: @scrollHandleHover; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; border: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QMenu { background: @surface; color: @text; border: 1px solid @border; border-radius: 8px; padding: 5px; }
QMenu::item { padding: 6px 24px 6px 12px; border-radius: 5px; }
QMenu::item:selected { background: @selection; }
QMenu::item:disabled { color: @textFaint; }
QMenu::separator { height: 1px; background: @border; margin: 5px 8px; }

QSplitter::handle { background: @window; }
QSplitter::handle:hover { background: @accentSoft; }
QSplitter::handle:horizontal { width: 5px; }
QSplitter::handle:vertical { height: 5px; }

QStatusBar { background: @window; color: @textDim; border-top: 1px solid @border; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: @textDim; padding: 0 6px; }
)");
    const std::pair<const char*, QColor> vars[] = {
        {"@window", t.window}, {"@surfaceAlt", t.surfaceAlt}, {"@surface", t.surface}, {"@card", t.card},
        {"@borderStrong", t.borderStrong}, {"@border", t.border}, {"@textDim", t.textDim}, {"@textFaint", t.textFaint},
        {"@text", t.text}, {"@accentHover", t.accentHover}, {"@accentText", t.accentText}, {"@accentSoft", t.accentSoft},
        {"@accent", t.accent}, {"@selection", t.selection}, {"@hover", t.hover}, {"@pressed", t.pressed},
        {"@tooltipBg", t.tooltipBg}, {"@tooltipText", t.tooltipText}, {"@scrollHandleHover", t.scrollHandleHover},
        {"@scrollHandle", t.scrollHandle}};
    // Longer names first, so "@borderStrong" isn't clobbered by "@border".
    for (const auto& [name, color] : vars)
        s.replace(QLatin1String(name), css(color));
    s.replace(QLatin1String("@mode"), mode);
    return s;
}

void ThemeManager::apply()
{
    const bool dark = mode_ == ThemeMode::Dark || (mode_ == ThemeMode::Auto && system_->prefersDark());
    if (applied_ && dark == appliedDark_)
        return;
    tokens_ = dark ? darkTokens() : lightTokens();
    if (!applied_)
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // Tell Qt (and the GTK platform theme's native dialogs) which scheme we use.
    QGuiApplication::styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
#endif
    QApplication::setPalette(paletteFor(tokens_));
    qApp->setStyleSheet(styleSheetFor(tokens_));
    applied_ = true;
    appliedDark_ = dark;
    emit themeChanged();
}

bool ThemeManager::eventFilter(QObject* o, QEvent* e)
{
    // Rounded menus need a translucent window behind the rounded border.
    if (e->type() == QEvent::Polish)
        if (auto* menu = qobject_cast<QMenu*>(o)) {
            menu->setAttribute(Qt::WA_TranslucentBackground);
            menu->setWindowFlag(Qt::FramelessWindowHint);
            menu->setWindowFlag(Qt::NoDropShadowWindowHint);
        }
    return QObject::eventFilter(o, e);
}

} // namespace culprit
