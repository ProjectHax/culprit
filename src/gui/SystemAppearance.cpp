// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/SystemAppearance.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleHints>

#include <cstdio>

namespace culprit {

namespace {

const QString kPortalService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPortalPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kSettingsIface = QStringLiteral("org.freedesktop.portal.Settings");

bool nameLooksDark(const QString& name) { return name.contains(QLatin1String("dark"), Qt::CaseInsensitive); }

// Runs a short helper (gsettings, xfconf-query) with a tight timeout.
std::optional<QString> runQuick(const QString& program, const QStringList& args)
{
    if (QStandardPaths::findExecutable(program).isEmpty())
        return std::nullopt;
    QProcess p;
    p.start(program, args);
    if (!p.waitForFinished(400) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        p.kill();
        return std::nullopt;
    }
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

QString desktop() { return qEnvironmentVariable("XDG_CURRENT_DESKTOP").toUpper(); }

QString configPath(const QString& rel)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QLatin1Char('/') + rel;
}

// The portal wraps values in variants (Read wraps twice); unwrap to the uint.
int unwrapScheme(QVariant v)
{
    for (int i = 0; i < 3 && v.canConvert<QDBusVariant>(); ++i)
        v = v.value<QDBusVariant>().variant();
    bool ok = false;
    const uint u = v.toUInt(&ok);
    return ok ? int(u) : -1;
}

} // namespace

SystemAppearance::SystemAppearance(const QPalette& startupPalette, QObject* parent)
    : QObject(parent), startupPalette_(startupPalette)
{
    QDBusConnection::sessionBus().connect(kPortalService, kPortalPath, kSettingsIface, QStringLiteral("SettingChanged"), this,
                                          SLOT(onPortalSettingChanged(QString, QString, QDBusVariant)));
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] { refresh(); });
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, [this] {
        watchFiles();   // editors and settings daemons replace files; re-add
        refresh();
    });
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] {
        watchFiles();
        refresh();
    });
    watchFiles();
    refresh();            // synchronous best guess so the first paint is already right
    queryPortalAsync();   // then the authoritative answer
}

void SystemAppearance::watchFiles()
{
    const QStringList files = {configPath(QStringLiteral("kdeglobals")), configPath(QStringLiteral("gtk-3.0/settings.ini")),
                               configPath(QStringLiteral("gtk-4.0/settings.ini"))};
    for (const QString& f : files)
        if (QFile::exists(f) && !watcher_.files().contains(f))
            watcher_.addPath(f);
}

void SystemAppearance::queryPortalAsync()
{
    // ReadOne (portal v2); older portals only have Read, handled on error.
    QDBusMessage msg = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kSettingsIface, QStringLiteral("ReadOne"));
    msg << QStringLiteral("org.freedesktop.appearance") << QStringLiteral("color-scheme");
    auto* w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg, 2000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        const QDBusMessage reply = w->reply();
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
            portalScheme_ = unwrapScheme(reply.arguments().constFirst());
            refresh();
            return;
        }
        if (reply.errorName() == QLatin1String("org.freedesktop.DBus.Error.UnknownMethod")) {
            QDBusMessage old = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kSettingsIface, QStringLiteral("Read"));
            old << QStringLiteral("org.freedesktop.appearance") << QStringLiteral("color-scheme");
            auto* w2 = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(old, 2000), this);
            connect(w2, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w2) {
                w2->deleteLater();
                const QDBusMessage r = w2->reply();
                if (r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty()) {
                    portalScheme_ = unwrapScheme(r.arguments().constFirst());
                    refresh();
                }
            });
        }
    });
}

void SystemAppearance::onPortalSettingChanged(const QString& ns, const QString& key, const QDBusVariant& value)
{
    if (ns == QLatin1String("org.freedesktop.appearance") && key == QLatin1String("color-scheme")) {
        portalScheme_ = unwrapScheme(QVariant::fromValue(value));
        refresh();
    } else if (ns == QLatin1String("org.gnome.desktop.interface") || ns == QLatin1String("org.kde.kdeglobals.General")) {
        refresh();   // theme name changes on desktops whose portal lacks color-scheme
    }
}

std::optional<bool> SystemAppearance::fromKde() const
{
    const QString path = configPath(QStringLiteral("kdeglobals"));
    if (!QFile::exists(path))
        return std::nullopt;
    QSettings kde(path, QSettings::IniFormat);
    const QStringList rgb = kde.value(QStringLiteral("Colors:Window/BackgroundNormal")).toStringList();
    if (rgb.size() >= 3)
        return QColor(rgb[0].toInt(), rgb[1].toInt(), rgb[2].toInt()).lightness() < 128;
    const QString scheme = kde.value(QStringLiteral("General/ColorScheme")).toString();
    if (!scheme.isEmpty())
        return nameLooksDark(scheme);
    return std::nullopt;
}

std::optional<bool> SystemAppearance::fromGsettings() const
{
    const QString d = desktop();
    struct Schema {
        const char* desktop;
        const char* schema;
    };
    static const Schema schemas[] = {{"CINNAMON", "org.cinnamon.desktop.interface"}, {"MATE", "org.mate.interface"},
                                     {"", "org.gnome.desktop.interface"}};
    for (const Schema& s : schemas) {
        if (*s.desktop && !d.contains(QLatin1String(s.desktop)))
            continue;
        if (QByteArray(s.schema) == "org.gnome.desktop.interface") {
            // GNOME 42+: explicit preference.
            if (auto v = runQuick(QStringLiteral("gsettings"), {QStringLiteral("get"), QLatin1String(s.schema), QStringLiteral("color-scheme")})) {
                if (v->contains(QLatin1String("prefer-dark")))
                    return true;
                if (v->contains(QLatin1String("prefer-light")))
                    return false;
            }
        }
        if (auto v = runQuick(QStringLiteral("gsettings"), {QStringLiteral("get"), QLatin1String(s.schema), QStringLiteral("gtk-theme")}))
            if (!v->isEmpty())
                return nameLooksDark(*v);
    }
    return std::nullopt;
}

std::optional<bool> SystemAppearance::fromXfce() const
{
    if (!desktop().contains(QLatin1String("XFCE")))
        return std::nullopt;
    if (auto v = runQuick(QStringLiteral("xfconf-query"), {QStringLiteral("-c"), QStringLiteral("xsettings"), QStringLiteral("-p"), QStringLiteral("/Net/ThemeName")}))
        if (!v->isEmpty())
            return nameLooksDark(*v);
    return std::nullopt;
}

std::optional<bool> SystemAppearance::fromGtk() const
{
    const QString env = qEnvironmentVariable("GTK_THEME");
    if (!env.isEmpty())
        return nameLooksDark(env);
    for (const char* rel : {"gtk-4.0/settings.ini", "gtk-3.0/settings.ini"}) {
        const QString path = configPath(QLatin1String(rel));
        if (!QFile::exists(path))
            continue;
        QSettings ini(path, QSettings::IniFormat);
        const QVariant prefer = ini.value(QStringLiteral("Settings/gtk-application-prefer-dark-theme"));
        if (prefer.isValid()) {
            const QString p = prefer.toString().toLower();
            if (p == QLatin1String("true") || p == QLatin1String("1"))
                return true;
        }
        const QString theme = ini.value(QStringLiteral("Settings/gtk-theme-name")).toString();
        if (!theme.isEmpty())
            return nameLooksDark(theme);
    }
    return std::nullopt;
}

std::optional<bool> SystemAppearance::fromQt() const
{
    // Note: our own ThemeManager also *requests* a scheme from Qt, so only the
    // unknown-before-first-apply state is meaningful; it is consulted last.
    const Qt::ColorScheme cs = QGuiApplication::styleHints()->colorScheme();
    if (cs == Qt::ColorScheme::Dark)
        return true;
    if (cs == Qt::ColorScheme::Light)
        return false;
    return std::nullopt;
}

void SystemAppearance::refresh()
{
    if (portalScheme_ == 1)
        return set(true, tr("desktop portal (color-scheme: dark)"));
    if (portalScheme_ == 2)
        return set(false, tr("desktop portal (color-scheme: light)"));
    // 0 = "no preference": fall through to the desktop's theme name.
    if (auto v = fromKde())
        return set(*v, tr("KDE colour scheme"));
    if (auto v = fromXfce())
        return set(*v, tr("XFCE theme"));
    if (auto v = fromGsettings())
        return set(*v, tr("GNOME/GTK desktop settings"));
    if (auto v = fromGtk())
        return set(*v, tr("GTK settings"));
    if (source_.isEmpty()) {   // only before we have applied our own palette
        if (auto v = fromQt())
            return set(*v, tr("Qt platform theme"));
        return set(startupPalette_.color(QPalette::Window).lightness() < 128, tr("system palette"));
    }
}

void SystemAppearance::set(bool dark, const QString& source)
{
    const bool changedValue = dark != dark_ || source_.isEmpty();
    dark_ = dark;
    source_ = source;
    if (qEnvironmentVariableIsSet("CULPRIT_DEBUG_THEME"))
        std::fprintf(stderr, "culprit: system appearance %s (from %s)\n", dark ? "dark" : "light", qPrintable(source));
    if (changedValue)
        emit changed(dark);
}

} // namespace culprit
