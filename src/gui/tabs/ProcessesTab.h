// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "gui/tabs/TabPage.h"

#include <QSortFilterProxyModel>

class QCheckBox;
class QLabel;
class QLineEdit;
class QTableWidget;
class QTextBrowser;
class QTreeView;

namespace culprit {

class Engine;
class ProcessModel;
struct ActionResult;

class ProcessFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void setText(const QString& text);
    void setShowKernelThreads(bool show);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    QString text_;
    bool showKernel_ = false;
};

class ProcessesTab : public TabPage {
    Q_OBJECT
public:
    ProcessesTab(Engine* engine, QWidget* parent = nullptr);

    void selectPid(int pid);

signals:
    void statusMessage(const QString& msg);
    void focusRequested(int pid, const QString& name);

protected:
    void accumulate(const Frame& f) override;
    void render(const Frame& f) override;

private:
    void showContextMenu(const QPoint& pos);
    void onSelectionChanged();
    void updateDetails(const Frame& f);
    void report(const ActionResult& r, const QString& action, const ProcKey& key, const QStringList& params);
    void runPrivileged(const QStringList& command);
    int selectedPid() const;

    void doSignal(const ProcSample& s, int sig);
    void doRenice(const ProcSample& s);
    void doIonice(const ProcSample& s);
    void doAffinity(const ProcSample& s);

    Engine* engine_;
    ProcessModel* model_;
    ProcessFilterProxy* proxy_;
    QTreeView* view_;
    QLineEdit* filter_;
    QCheckBox* tree_;
    QCheckBox* kernel_;
    QLabel* summary_;
    QTextBrowser* info_;
    QTableWidget* threads_;
    int detailPid_ = 0;
    bool firstFrame_ = true;
};

} // namespace culprit
