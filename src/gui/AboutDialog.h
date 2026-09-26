// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "gui/widgets/WindowChrome.h"

class QTabWidget;
class QUrl;

namespace culprit {

// Version, copyright and license information: Culprit's GPL, the third-party
// notices, the LGPL that Qt is used under and, in packages that bundle Qt,
// the notices of every bundled library.
class AboutDialog : public ChromeDialog {
    Q_OBJECT
public:
    explicit AboutDialog(QWidget* parent);

private:
    void openLink(const QUrl& url);

    QTabWidget* tabs_ = nullptr;
    int licenseTab_ = -1;
    int lgplTab_ = -1;
};

} // namespace culprit
