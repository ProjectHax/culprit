// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include <QDBusVariant>
#include <QFileSystemWatcher>
#include <QObject>
#include <QPalette>
#include <QString>

#include <optional>

namespace culprit {

// Works out whether the desktop prefers a dark appearance, on any desktop
// environment, and notices when that changes.
//
// Sources, most authoritative first:
//   1. xdg-desktop-portal Settings "org.freedesktop.appearance color-scheme"
//      (GNOME, KDE, COSMIC, Cinnamon, most wlroots setups) — live via SettingChanged
//   2. KDE: kdeglobals window background colour
//   3. GNOME / Budgie / Cinnamon / MATE: gsettings color-scheme or GTK theme name
//   4. XFCE: xfconf xsettings theme name
//   5. GTK_THEME, gtk-4.0/gtk-3.0 settings.ini (prefer-dark / theme name)
//   6. Qt's own colour-scheme hint, then the startup palette's brightness
class SystemAppearance : public QObject {
    Q_OBJECT
public:
    explicit SystemAppearance(const QPalette& startupPalette, QObject* parent = nullptr);

    bool prefersDark() const { return dark_; }
    QString source() const { return source_; }   // human readable, e.g. "xdg-desktop-portal"
    void refresh();

signals:
    void changed(bool dark);

private slots:
    void onPortalSettingChanged(const QString& ns, const QString& key, const QDBusVariant& value);

private:
    void queryPortalAsync();
    void set(bool dark, const QString& source);
    std::optional<bool> fromKde() const;
    std::optional<bool> fromGsettings() const;
    std::optional<bool> fromXfce() const;
    std::optional<bool> fromGtk() const;
    std::optional<bool> fromQt() const;
    void watchFiles();

    QPalette startupPalette_;
    QFileSystemWatcher watcher_;
    int portalScheme_ = -1;   // 0 no preference, 1 dark, 2 light, -1 unknown
    bool dark_ = false;
    QString source_;
};

} // namespace culprit
