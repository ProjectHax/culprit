// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/AboutDialog.h"

#include "core/Version.h"
#include "gui/Theme.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

#ifndef CULPRIT_DOCDIR_FROM_BINDIR
#define CULPRIT_DOCDIR_FROM_BINDIR "../share/doc/culprit"
#endif

namespace culprit {

namespace {

constexpr auto kProjectUrl = "https://github.com/ProjectHax/culprit";

QString readText(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

QTextBrowser* plainTextView(const QString& text)
{
    auto* view = new QTextBrowser;
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    view->setLineWrapMode(QTextEdit::NoWrap);
    view->setPlainText(text);
    return view;
}

// Notices for the libraries a .deb or AppImage bundles, generated at packaging
// time next to the documentation (…/share/doc/culprit).
QString bundledNoticesPath()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/" CULPRIT_DOCDIR_FROM_BINDIR "/THIRD-PARTY-BUNDLED.txt");
}

} // namespace

AboutDialog::AboutDialog(QWidget* parent) : ChromeDialog(parent, tr("About Culprit"))
{
    QVBoxLayout* v = body();

    auto* head = new QHBoxLayout;
    auto* icon = new QLabel;
    icon->setPixmap(QIcon(QStringLiteral(":/culprit.svg")).pixmap(QSize(64, 64), devicePixelRatio()));
    icon->setAlignment(Qt::AlignTop);
    head->addWidget(icon);
    auto* title = new QLabel(tr("<span style='font-size:x-large; font-weight:600'>Culprit</span><br>"
                                "Version %1<br>Finds the culprit behind load, heat and stutter on Linux.")
                                 .arg(QString::fromLatin1(kVersion)));
    head->addWidget(title, 1);
    v->addLayout(head);

    auto* notice = new QLabel(tr("Copyright © 2026 ProjectHax LLC<br><br>"
                                 "Culprit is free software: you can redistribute it and/or modify it under the terms "
                                 "of the GNU General Public License as published by the Free Software Foundation, "
                                 "either version 3 of the License, or (at your option) any later version. It comes "
                                 "with ABSOLUTELY NO WARRANTY.<br><br><a href='%1'>%2</a>")
                                  .arg(QString::fromLatin1(kProjectUrl), QString::fromLatin1(kProjectUrl).mid(8)));
    notice->setWordWrap(true);
    notice->setOpenExternalLinks(true);
    v->addWidget(notice);

    auto* build = new QLabel(tr("Qt %1 (built with %2)").arg(QString::fromLatin1(qVersion()), QStringLiteral(QT_VERSION_STR)));
    QPalette dim = build->palette();
    dim.setColor(QPalette::WindowText, theme::dimText(dim));
    build->setPalette(dim);
    v->addWidget(build);

    tabs_ = new QTabWidget;
    tabs_->setDocumentMode(true);
    licenseTab_ = tabs_->addTab(plainTextView(readText(QStringLiteral(":/legal/LICENSE"))), tr("License"));

    auto* thirdParty = new QTextBrowser;
    thirdParty->setOpenLinks(false);
    thirdParty->setMarkdown(readText(QStringLiteral(":/legal/THIRD-PARTY-NOTICES.md")));
    connect(thirdParty, &QTextBrowser::anchorClicked, this, &AboutDialog::openLink);
    tabs_->addTab(thirdParty, tr("Third-party software"));
    lgplTab_ = tabs_->addTab(plainTextView(readText(QStringLiteral(":/legal/licenses/LGPL-3.0-only.txt"))), tr("LGPL 3.0 (Qt)"));

    const QString bundled = readText(bundledNoticesPath());
    if (!bundled.isEmpty())
        tabs_->addTab(plainTextView(bundled), tr("Bundled libraries"));
    v->addWidget(tabs_, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setObjectName(QStringLiteral("primary"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);

    resize(760, 680);
}

void AboutDialog::openLink(const QUrl& url)
{
    // The notices link to files of the source tree; show the matching tab instead.
    const QString path = url.path();
    if (url.isRelative() && path == QLatin1String("LICENSE"))
        tabs_->setCurrentIndex(licenseTab_);
    else if (url.isRelative() && path.endsWith(QLatin1String("LGPL-3.0-only.txt")))
        tabs_->setCurrentIndex(lgplTab_);
    else if (!url.isRelative())
        QDesktopServices::openUrl(url);
}

} // namespace culprit
