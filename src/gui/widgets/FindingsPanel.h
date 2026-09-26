// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/model/Frame.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QListWidget;
class QTextBrowser;

namespace culprit {

// The "diagnosis" list: active findings (and recently cleared ones), with the
// selected finding's explanation, evidence, suspects and advice.
class FindingsPanel : public QWidget {
    Q_OBJECT
public:
    explicit FindingsPanel(QWidget* parent = nullptr);
    void setFindings(const std::vector<Finding>& findings);
    QString selectedId() const { return selectedId_; }

signals:
    void processActivated(int pid);

private:
    void showDetails(const Finding* f);

    QListWidget* list_;
    QTextBrowser* details_;
    QCheckBox* showCleared_;
    QLabel* header_;
    std::vector<Finding> findings_;
    QString selectedId_;
    QString autoSelected_;   // set while the selection follows the top finding
};

QString findingHtml(const Finding& f, const QPalette& pal);

} // namespace culprit
