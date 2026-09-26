// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/model/Frame.h"

#include <QAbstractItemModel>

#include <memory>
#include <unordered_map>
#include <vector>

namespace culprit {

// Process table/tree. Updated incrementally from each Frame: rows are inserted,
// moved (re-parenting) and removed individually so views keep their selection,
// expansion and scroll position.
class ProcessModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Column {
        ColName, ColPid, ColUser, ColCpu, ColRunDelay, ColInvCtx, ColMajFlt, ColRss, ColThreads,
        ColGpu, ColPower, ColIoRead, ColIoWrite, ColState, ColNice, ColUnit, ColCommand, ColCount
    };
    enum Roles { SortRole = Qt::UserRole + 1, PidRole, KernelThreadRole, FilterTextRole };

    explicit ProcessModel(QObject* parent = nullptr);
    ~ProcessModel() override;

    void setTreeMode(bool tree);
    bool treeMode() const { return tree_; }
    void update(const FramePtr& frame);

    const ProcSample* sample(const QModelIndex& index) const;
    QModelIndex indexForPid(int pid, int column = 0) const;

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    static QString columnTooltip(int column);

private:
    struct Node {
        ProcKey key;
        ProcSample sample;
        Node* parent = nullptr;
        std::vector<Node*> children;
        int row = 0;
    };

    QModelIndex indexOf(const Node* n, int column = 0) const;
    Node* nodeOf(const QModelIndex& idx) const;
    Node* desiredParent(const Node* n) const;
    bool isAncestor(const Node* maybeAncestor, const Node* n) const;
    void renumber(Node* parent, int from);
    void rebuild(const FramePtr& frame);
    void collectSubtree(Node* n, std::vector<ProcKey>& keys) const;
    void emitChanged(Node* parent);

    Node root_;
    std::unordered_map<ProcKey, std::unique_ptr<Node>, ProcKeyHash> nodes_;
    std::unordered_map<int, Node*> byPid_;
    bool tree_ = true;
    bool needRebuild_ = true;
    FramePtr frame_;
    double totalMemBytes_ = 0;
};

} // namespace culprit
