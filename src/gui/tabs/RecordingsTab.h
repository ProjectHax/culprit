// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/record/RecordingReader.h"
#include "gui/tabs/TabPage.h"

class QLabel;
class QListWidget;
class QPushButton;
class QTabWidget;
class QTextBrowser;

namespace culprit {

class DataTable;
class TimelineView;

// Browse and replay recordings made with the Record button or `culprit --record`.
class RecordingsTab : public TabPage {
    Q_OBJECT
public:
    RecordingsTab(const QString& dir, QWidget* parent = nullptr);
    void setDirectory(const QString& dir);
    void refresh();
    bool openFile(const QString& path);

signals:
    void statusMessage(const QString& msg);

protected:
    void render(const Frame&) override {}
    void showEvent(QShowEvent* e) override;

private:
    void showRecording();
    void showHitch(qint64 id);
    void showFinding(int index);
    void exportReport(bool json);

    QString dir_;
    Recording rec_;
    bool loaded_ = false;
    QListWidget* files_;
    QLabel* header_;
    TimelineView* top_;
    TimelineView* bottom_;
    QTabWidget* lists_;
    DataTable* hitches_;
    DataTable* findings_;
    QTextBrowser* detail_;
    QPushButton* exportText_;
    QPushButton* exportJson_;
};

} // namespace culprit
