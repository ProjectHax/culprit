// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/models/ProcessModel.h"

#include "core/Format.h"
#include "gui/Theme.h"

#include <QBrush>
#include <QColor>

#include <algorithm>
#include <unordered_set>

namespace culprit {

ProcessModel::ProcessModel(QObject* parent) : QAbstractItemModel(parent) {}

ProcessModel::~ProcessModel() = default;

void ProcessModel::setTreeMode(bool tree)
{
    if (tree == tree_)
        return;
    tree_ = tree;
    needRebuild_ = true;
    if (frame_)
        update(frame_);
}

// ------------------------------------------------------------------ structure

QModelIndex ProcessModel::indexOf(const Node* n, int column) const
{
    if (!n || n == &root_)
        return {};
    return createIndex(n->row, column, const_cast<Node*>(n));
}

ProcessModel::Node* ProcessModel::nodeOf(const QModelIndex& idx) const
{
    return idx.isValid() ? static_cast<Node*>(idx.internalPointer()) : const_cast<Node*>(&root_);
}

QModelIndex ProcessModel::index(int row, int column, const QModelIndex& parent) const
{
    const Node* p = nodeOf(parent);
    if (row < 0 || row >= int(p->children.size()) || column < 0 || column >= ColCount)
        return {};
    return createIndex(row, column, p->children[size_t(row)]);
}

QModelIndex ProcessModel::parent(const QModelIndex& child) const
{
    if (!child.isValid())
        return {};
    return indexOf(nodeOf(child)->parent);
}

int ProcessModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid() && parent.column() != 0)
        return 0;
    return int(nodeOf(parent)->children.size());
}

int ProcessModel::columnCount(const QModelIndex&) const { return ColCount; }

const ProcSample* ProcessModel::sample(const QModelIndex& index) const
{
    if (!index.isValid())
        return nullptr;
    return &nodeOf(index)->sample;
}

QModelIndex ProcessModel::indexForPid(int pid, int column) const
{
    auto it = byPid_.find(pid);
    return it == byPid_.end() ? QModelIndex() : indexOf(it->second, column);
}

ProcessModel::Node* ProcessModel::desiredParent(const Node* n) const
{
    if (!tree_)
        return const_cast<Node*>(&root_);
    auto it = byPid_.find(n->sample.ppid);
    if (it == byPid_.end() || it->second == n)
        return const_cast<Node*>(&root_);
    // A parent must have started before its child; otherwise ppid refers to a
    // recycled pid and the real parent is gone.
    if (it->second->key.starttime > n->key.starttime)
        return const_cast<Node*>(&root_);
    return it->second;
}

bool ProcessModel::isAncestor(const Node* maybeAncestor, const Node* n) const
{
    for (const Node* p = n; p; p = p->parent)
        if (p == maybeAncestor)
            return true;
    return false;
}

void ProcessModel::renumber(Node* parent, int from)
{
    for (size_t i = size_t(std::max(0, from)); i < parent->children.size(); ++i)
        parent->children[i]->row = int(i);
}

void ProcessModel::collectSubtree(Node* n, std::vector<ProcKey>& keys) const
{
    keys.push_back(n->key);
    for (Node* c : n->children)
        collectSubtree(c, keys);
}

// ------------------------------------------------------------------ updates

void ProcessModel::rebuild(const FramePtr& frame)
{
    beginResetModel();
    nodes_.clear();
    byPid_.clear();
    root_.children.clear();
    for (const ProcSample& s : frame->procs) {
        auto node = std::make_unique<Node>();
        node->key = s.key;
        node->sample = s;
        byPid_[s.key.pid] = node.get();
        nodes_[s.key] = std::move(node);
    }
    for (auto& [key, node] : nodes_) {
        Node* p = desiredParent(node.get());
        if (p != &root_ && isAncestor(node.get(), p))
            p = &root_;
        node->parent = p;
        node->row = int(p->children.size());
        p->children.push_back(node.get());
    }
    endResetModel();
    needRebuild_ = false;
}

void ProcessModel::update(const FramePtr& frame)
{
    frame_ = frame;
    totalMemBytes_ = double(frame->sys.mem.totalKb) * 1024.0;
    if (needRebuild_ || nodes_.empty()) {
        rebuild(frame);
        return;
    }

    std::unordered_map<ProcKey, const ProcSample*, ProcKeyHash> incoming;
    incoming.reserve(frame->procs.size());
    for (const ProcSample& s : frame->procs)
        incoming[s.key] = &s;

    // 1. Refresh pid -> node for everything alive (existing + new) so parents resolve.
    byPid_.clear();
    std::vector<Node*> added;
    for (const ProcSample& s : frame->procs) {
        auto it = nodes_.find(s.key);
        Node* n;
        if (it == nodes_.end()) {
            auto node = std::make_unique<Node>();
            node->key = s.key;
            n = node.get();
            nodes_[s.key] = std::move(node);
            added.push_back(n);
        } else {
            n = it->second.get();
        }
        n->sample = s;
        byPid_[s.key.pid] = n;
    }

    // 2. Insert new nodes directly under their parent (parents first: by start time).
    std::sort(added.begin(), added.end(), [](Node* a, Node* b) { return a->key.starttime < b->key.starttime; });
    std::unordered_set<Node*> addedSet(added.begin(), added.end());
    for (Node* n : added) {
        Node* p = desiredParent(n);
        if (p != &root_ && addedSet.count(p) && !p->parent)
            p = &root_;   // parent not inserted yet (shouldn't happen after sorting)
        const int row = int(p->children.size());
        beginInsertRows(indexOf(p), row, row);
        n->parent = p;
        n->row = row;
        p->children.push_back(n);
        endInsertRows();
    }

    // 3. Re-parent live nodes whose parent changed (e.g. parent died -> reparented to init).
    for (const ProcSample& s : frame->procs) {
        Node* n = nodes_[s.key].get();
        if (addedSet.count(n))
            continue;
        Node* want = desiredParent(n);
        if (want != &root_ && !incoming.count(want->key))
            want = &root_;
        if (want == n->parent || (want != &root_ && isAncestor(n, want)))
            continue;
        Node* from = n->parent;
        const int srcRow = n->row;
        const int dstRow = int(want->children.size());
        if (!beginMoveRows(indexOf(from), srcRow, srcRow, indexOf(want), dstRow))
            continue;
        from->children.erase(from->children.begin() + srcRow);
        renumber(from, srcRow);
        n->parent = want;
        n->row = int(want->children.size());
        want->children.push_back(n);
        endMoveRows();
    }

    // 4. Remove dead nodes (their live children were moved away in step 3).
    std::vector<Node*> dead;
    for (auto& [key, node] : nodes_)
        if (!incoming.count(key))
            dead.push_back(node.get());
    // Remove deepest first so a dead parent's dead children go before it.
    std::sort(dead.begin(), dead.end(), [](Node* a, Node* b) {
        int da = 0, db = 0;
        for (Node* p = a; p; p = p->parent) ++da;
        for (Node* p = b; p; p = p->parent) ++db;
        return da > db;
    });
    for (Node* n : dead) {
        Node* from = n->parent;
        // Dead children were removed first (deepest-first order), so anything left
        // is alive but wasn't re-parented in step 3: move it to the top level.
        while (!n->children.empty()) {
            Node* c = n->children.back();
            const int dstRow = int(root_.children.size());
            beginMoveRows(indexOf(n), c->row, c->row, QModelIndex(), dstRow);
            n->children.pop_back();
            c->parent = &root_;
            c->row = dstRow;
            root_.children.push_back(c);
            endMoveRows();
        }
        beginRemoveRows(indexOf(from), n->row, n->row);
        from->children.erase(from->children.begin() + n->row);
        renumber(from, n->row);
        endRemoveRows();
        byPid_.erase(n->key.pid);
        nodes_.erase(n->key);
    }
    // byPid_ may have lost entries that pointed at live nodes with the same pid.
    for (auto& [key, node] : nodes_)
        byPid_[key.pid] = node.get();

    // 5. Values changed everywhere.
    emitChanged(&root_);
}

void ProcessModel::emitChanged(Node* parent)
{
    if (parent->children.empty())
        return;
    emit dataChanged(index(0, 0, indexOf(parent)), index(int(parent->children.size()) - 1, ColCount - 1, indexOf(parent)));
    for (Node* c : parent->children)
        emitChanged(c);
}

// ------------------------------------------------------------------ data

QString ProcessModel::columnTooltip(int column)
{
    switch (column) {
    case ColCpu: return tr("CPU usage in % of one core (can exceed 100% for multi-threaded processes).");
    case ColRunDelay: return tr("Run-queue wait: time the process's threads were runnable but waiting for a CPU, "
                                "in ms per second. High values mean the process is being starved/delayed.\n"
                                "Measured for busy processes (above 5% of a core, or 1% while CPUs are contended).");
    case ColInvCtx: return tr("Involuntary context switches per second: how often the process was preempted.\n"
                              "Measured for the selected process, and for the busiest ones while CPUs are contended.");
    case ColMajFlt: return tr("Major page faults per second (reads from disk/swap to satisfy memory accesses).");
    case ColGpu: return tr("GPU SM utilisation attributed to this process (NVIDIA only).");
    case ColPower: return tr("Estimated power: CPU core power split by CPU time × frequency, plus GPU power split "
                             "by GPU utilisation. An estimate, not a measurement.");
    case ColIoRead: return tr("Storage read throughput (own processes, or all with Deep trace).");
    case ColIoWrite: return tr("Storage write throughput (own processes, or all with Deep trace).");
    case ColState: return tr("R running, S sleeping, D uninterruptible (usually I/O), Z zombie, T stopped, I idle kernel thread.");
    case ColUnit: return tr("systemd unit / scope the process belongs to (from its cgroup).");
    default: return {};
    }
}

QVariant ProcessModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal)
        return {};
    if (role == Qt::ToolTipRole)
        return columnTooltip(section);
    if (role == Qt::TextAlignmentRole)
        return section == ColName || section == ColUser || section == ColUnit || section == ColCommand || section == ColState
                   ? int(Qt::AlignLeft | Qt::AlignVCenter)
                   : int(Qt::AlignRight | Qt::AlignVCenter);
    if (role != Qt::DisplayRole)
        return {};
    switch (section) {
    case ColName: return tr("Name");
    case ColPid: return tr("PID");
    case ColUser: return tr("User");
    case ColCpu: return tr("CPU");
    case ColRunDelay: return tr("RQ wait");
    case ColInvCtx: return tr("Preempt/s");
    case ColMajFlt: return tr("Maj flt/s");
    case ColRss: return tr("Memory");
    case ColThreads: return tr("Thr");
    case ColGpu: return tr("GPU");
    case ColPower: return tr("Est. W");
    case ColIoRead: return tr("Read/s");
    case ColIoWrite: return tr("Write/s");
    case ColState: return tr("State");
    case ColNice: return tr("Nice");
    case ColUnit: return tr("Unit");
    case ColCommand: return tr("Command");
    }
    return {};
}

namespace {

QString policyName(int policy)
{
    switch (policy) {
    case 1: return QStringLiteral("FIFO");
    case 2: return QStringLiteral("RR");
    case 3: return QStringLiteral("BATCH");
    case 5: return QStringLiteral("IDLE");
    case 6: return QStringLiteral("DL");
    default: return {};
    }
}

} // namespace

QVariant ProcessModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return {};
    const ProcSample& s = nodeOf(index)->sample;
    const int col = index.column();

    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        case ColName: return s.comm;
        case ColPid: return s.key.pid;
        case ColUser: return s.user;
        case ColCpu: return s.cpuPct >= 0.05 ? QString::number(s.cpuPct, 'f', 1) : QString();
        case ColRunDelay: return s.runDelayMsPs >= 0.05 ? QString::number(s.runDelayMsPs, 'f', 1) : QString();
        case ColInvCtx: return s.nivcswPs >= 0.5 ? fmt::count(s.nivcswPs) : QString();
        case ColMajFlt: return s.majfltPs >= 0.5 ? fmt::count(s.majfltPs) : QString();
        case ColRss: return s.rssBytes ? fmt::bytes(double(s.rssBytes)) : QString();
        case ColThreads: return s.threads;
        case ColGpu: return s.gpuPct >= 0.5 ? QString::number(s.gpuPct, 'f', 0) + QLatin1Char('%') : QString();
        case ColPower: {
            const double w = std::max(0.0, s.estCpuW) + std::max(0.0, s.estGpuW);
            return w >= 0.05 ? QString::number(w, 'f', 1) : QString();
        }
        case ColIoRead: return s.ioReadBps >= 1 ? fmt::bytesPerSec(s.ioReadBps) : QString();
        case ColIoWrite: return s.ioWriteBps >= 1 ? fmt::bytesPerSec(s.ioWriteBps) : QString();
        case ColState: {
            QString st = QString(QLatin1Char(s.state));
            if (s.dThreads > 0 && s.state != 'D')
                st += QStringLiteral(" (%1 D)").arg(s.dThreads);
            if (!s.wchan.isEmpty())
                st += QStringLiteral(" ") + s.wchan;
            return st;
        }
        case ColNice: {
            const QString pol = policyName(s.policy);
            if (!pol.isEmpty())
                return s.policy == 1 || s.policy == 2 ? QStringLiteral("%1 %2").arg(pol).arg(s.rtPrio) : pol;
            return s.nice;
        }
        case ColUnit: return s.unit;
        case ColCommand: return s.cmdline.isEmpty() ? QStringLiteral("[%1]").arg(s.comm) : s.cmdline;
        }
        break;
    case SortRole:
        switch (col) {
        case ColName: return s.comm.toLower();
        case ColPid: return s.key.pid;
        case ColUser: return s.user;
        case ColCpu: return s.cpuPct;
        case ColRunDelay: return s.runDelayMsPs;
        case ColInvCtx: return s.nivcswPs;
        case ColMajFlt: return s.majfltPs;
        case ColRss: return qulonglong(s.rssBytes);
        case ColThreads: return s.threads;
        case ColGpu: return s.gpuPct;
        case ColPower: return std::max(0.0, s.estCpuW) + std::max(0.0, s.estGpuW);
        case ColIoRead: return s.ioReadBps;
        case ColIoWrite: return s.ioWriteBps;
        case ColState: return QString(QLatin1Char(s.state));
        case ColNice: return s.policy == 1 || s.policy == 2 ? -100 - s.rtPrio : s.nice;
        case ColUnit: return s.unit;
        case ColCommand: return s.cmdline;
        }
        break;
    case Qt::TextAlignmentRole:
        return col == ColName || col == ColUser || col == ColUnit || col == ColCommand || col == ColState
                   ? int(Qt::AlignLeft | Qt::AlignVCenter)
                   : int(Qt::AlignRight | Qt::AlignVCenter);
    case Qt::ForegroundRole:
        if (col == ColState && s.state == 'D')
            return QBrush(theme::amber());
        if (col == ColRunDelay && s.runDelayMsPs >= 50)
            return QBrush(theme::red());
        if (col == ColNice && (s.policy == 1 || s.policy == 2))
            return QBrush(theme::purple());
        if (s.kernelThread && col == ColName)
            return QBrush(QColor(0x88, 0x88, 0x88));
        break;
    case Qt::BackgroundRole:
        if (col == ColCpu && s.cpuPct >= 5) {
            QColor c = theme::heat(std::min(1.0, s.cpuPct / 400.0));
            c.setAlpha(int(40 + std::min(1.0, s.cpuPct / 100.0) * 70));
            return QBrush(c);
        }
        if (col == ColRss && totalMemBytes_ > 0 && double(s.rssBytes) / totalMemBytes_ > 0.05) {
            QColor c = theme::heat(std::min(1.0, double(s.rssBytes) / totalMemBytes_ * 4));
            c.setAlpha(60);
            return QBrush(c);
        }
        break;
    case Qt::ToolTipRole:
        if (col == ColName || col == ColCommand)
            return QStringLiteral("%1 (pid %2)\n%3\ncgroup: %4").arg(s.comm).arg(s.key.pid).arg(s.cmdline, s.cgroup);
        return columnTooltip(col);
    case PidRole:
        return s.key.pid;
    case KernelThreadRole:
        return s.kernelThread;
    case FilterTextRole:
        return QStringLiteral("%1 %2 %3 %4 %5").arg(s.comm).arg(s.key.pid).arg(s.user, s.unit, s.cmdline);
    }
    return {};
}

} // namespace culprit
