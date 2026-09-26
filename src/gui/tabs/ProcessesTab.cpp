// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/tabs/ProcessesTab.h"

#include "core/Engine.h"
#include "core/Format.h"
#include "core/actions/ProcessActions.h"
#include "gui/Dialogs.h"
#include "gui/models/ProcessModel.h"
#include "gui/widgets/WindowChrome.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTreeView>
#include <QVBoxLayout>

#include <csignal>

namespace culprit {

// ------------------------------------------------------------------ proxy

void ProcessFilterProxy::setText(const QString& text)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    text_ = text.trimmed();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    text_ = text.trimmed();
    invalidateRowsFilter();
#endif
}

void ProcessFilterProxy::setShowKernelThreads(bool show)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    showKernel_ = show;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    showKernel_ = show;
    invalidateRowsFilter();
#endif
}

bool ProcessFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
    if (!showKernel_ && idx.data(ProcessModel::KernelThreadRole).toBool())
        return false;
    if (text_.isEmpty())
        return true;
    return idx.data(ProcessModel::FilterTextRole).toString().contains(text_, Qt::CaseInsensitive);
}

// ------------------------------------------------------------------ tab

ProcessesTab::ProcessesTab(Engine* engine, QWidget* parent) : TabPage(parent), engine_(engine)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);

    auto* bar = new QHBoxLayout;
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter…"));
    filter_->setToolTip(tr("Matches name, PID, user, systemd unit or command line"));
    filter_->setClearButtonEnabled(true);
    tree_ = new QCheckBox(tr("Tree"));
    tree_->setChecked(false);
    kernel_ = new QCheckBox(tr("Kernel threads"));
    summary_ = new QLabel;
    bar->addWidget(filter_, 1);
    bar->addWidget(tree_);
    bar->addWidget(kernel_);
    bar->addSpacing(12);
    bar->addWidget(summary_);
    layout->addLayout(bar);

    model_ = new ProcessModel(this);
    model_->setTreeMode(false);
    proxy_ = new ProcessFilterProxy(this);
    proxy_->setSourceModel(model_);
    proxy_->setSortRole(ProcessModel::SortRole);
    proxy_->setRecursiveFilteringEnabled(true);
    proxy_->setDynamicSortFilter(true);

    auto* split = new QSplitter(Qt::Vertical);
    view_ = new QTreeView;
    view_->setModel(proxy_);
    view_->setSortingEnabled(true);
    view_->sortByColumn(ProcessModel::ColCpu, Qt::DescendingOrder);
    view_->setUniformRowHeights(true);
    view_->setAlternatingRowColors(true);
    view_->setRootIsDecorated(false);
    view_->setSelectionMode(QAbstractItemView::SingleSelection);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    view_->header()->setStretchLastSection(true);
    view_->header()->setSectionsMovable(true);
    const int widths[] = {170, 82, 80, 60, 70, 75, 70, 85, 45, 50, 60, 80, 80, 110, 55, 200, 300};
    for (int c = 0; c < ProcessModel::ColCount; ++c)
        view_->setColumnWidth(c, widths[c]);
    split->addWidget(view_);

    auto* details = new QSplitter(Qt::Horizontal);
    info_ = new QTextBrowser;
    info_->setOpenLinks(false);
    threads_ = new QTableWidget(0, 7);
    threads_->setHorizontalHeaderLabels(
        {tr("TID"), tr("Thread"), tr("State"), tr("CPU %"), tr("RQ wait ms/s"), tr("Preempt/s"), tr("Last CPU")});
    threads_->verticalHeader()->hide();
    threads_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    threads_->setSelectionBehavior(QAbstractItemView::SelectRows);
    threads_->horizontalHeader()->setStretchLastSection(true);
    threads_->setSortingEnabled(true);
    details->addWidget(info_);
    details->addWidget(threads_);
    details->setStretchFactor(0, 2);
    details->setStretchFactor(1, 3);
    split->addWidget(details);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    layout->addWidget(split, 1);

    info_->setPlaceholderText(tr("Select a process."));

    connect(filter_, &QLineEdit::textChanged, proxy_, &ProcessFilterProxy::setText);
    connect(kernel_, &QCheckBox::toggled, proxy_, &ProcessFilterProxy::setShowKernelThreads);
    connect(tree_, &QCheckBox::toggled, this, [this](bool on) {
        model_->setTreeMode(on);
        view_->setRootIsDecorated(on);
        if (on)
            view_->expandAll();
    });
    connect(view_, &QTreeView::customContextMenuRequested, this, &ProcessesTab::showContextMenu);
    connect(view_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, &ProcessesTab::onSelectionChanged);
}

int ProcessesTab::selectedPid() const
{
    const QModelIndex cur = view_->currentIndex();
    return cur.isValid() ? cur.data(ProcessModel::PidRole).toInt() : 0;
}

void ProcessesTab::selectPid(int pid)
{
    const QModelIndex src = model_->indexForPid(pid);
    const QModelIndex idx = proxy_->mapFromSource(src);
    if (idx.isValid()) {
        view_->setCurrentIndex(idx);
        view_->scrollTo(idx);
    }
}

void ProcessesTab::onSelectionChanged()
{
    const int pid = selectedPid();
    if (pid == detailPid_)
        return;
    detailPid_ = pid;
    engine_->setDetailPids(pid ? QList<int>{pid} : QList<int>{});
    threads_->setRowCount(0);
    if (lastFrame())
        updateDetails(*lastFrame());
}

void ProcessesTab::accumulate(const Frame& f)
{
    // The model is cheap to keep current and keeps selection stable.
    model_->update(lastFrame());
    if (firstFrame_ && tree_->isChecked())
        view_->expandAll();
    firstFrame_ = false;
    int threads = 0;
    for (const ProcSample& p : f.procs)
        threads += p.threads;
    summary_->setText(tr("%1 processes · %2 threads").arg(f.procs.size()).arg(threads));
}

void ProcessesTab::render(const Frame& f) { updateDetails(f); }

void ProcessesTab::updateDetails(const Frame& f)
{
    const ProcSample* s = detailPid_ ? f.findProc(detailPid_) : nullptr;
    if (!s) {
        if (detailPid_)
            info_->setHtml(tr("<i>Process %1 has exited.</i>").arg(detailPid_));
        return;
    }
    auto row = [](const QString& k, const QString& v) {
        return QStringLiteral("<tr><td style='color:gray;padding-right:10px'>%1</td><td>%2</td></tr>").arg(k, v.toHtmlEscaped());
    };
    QString html = QStringLiteral("<h3 style='margin:0'>%1 <span style='color:gray'>(%2)</span></h3><table>")
                       .arg(s->comm.toHtmlEscaped())
                       .arg(s->key.pid);
    html += row(tr("Command"), s->cmdline.isEmpty() ? tr("[kernel thread]") : s->cmdline);
    html += row(tr("User"), s->user);
    html += row(tr("Parent PID"), QString::number(s->ppid));
    html += row(tr("cgroup"), s->cgroup);
    html += row(tr("CPU"), fmt::percent(s->cpuPct) + tr(" of one core, last on CPU %1").arg(s->lastCpu));
    html += row(tr("Run-queue wait"), s->runDelayMsPs >= 0 ? tr("%1 ms per second").arg(s->runDelayMsPs, 0, 'f', 1) : tr("not measured (not busy)"));
    html += row(tr("Preemptions"), s->nivcswPs >= 0 ? fmt::rate(s->nivcswPs) : QStringLiteral("–"));
    html += row(tr("Page faults"), tr("%1 major, %2 minor").arg(fmt::rate(s->majfltPs), fmt::rate(s->minfltPs)));
    html += row(tr("Memory (RSS)"), fmt::bytes(double(s->rssBytes)));
    if (s->ioReadBps >= 0)
        html += row(tr("Storage I/O"), tr("read %1, write %2").arg(fmt::bytesPerSec(s->ioReadBps), fmt::bytesPerSec(s->ioWriteBps)));
    if (s->blkioMsPs >= 0)
        html += row(tr("Block I/O wait"), tr("%1 ms/s").arg(s->blkioMsPs, 0, 'f', 1));
    if (s->gpuPct >= 0)
        html += row(tr("GPU"), tr("%1% SM, %2 memory").arg(s->gpuPct, 0, 'f', 0).arg(fmt::bytes(double(s->gpuMemBytes))));
    if (s->estCpuW >= 0 || s->estGpuW >= 0)
        html += row(tr("Est. power"), tr("CPU %1, GPU %2").arg(fmt::watts(s->estCpuW), fmt::watts(s->estGpuW)));
    html += row(tr("Scheduling"), s->policy == 1 || s->policy == 2 ? tr("real-time %1 prio %2").arg(s->policy == 1 ? QStringLiteral("FIFO") : QStringLiteral("RR")).arg(s->rtPrio)
                                                                  : tr("nice %1").arg(s->nice));
    QStringList aff;
    for (int c : ProcessActions::affinity(s->key.pid))
        aff << QString::number(c);
    html += row(tr("CPU affinity"), aff.size() == f.sys.onlineCpus ? tr("all CPUs") : aff.join(QLatin1Char(',')));
    html += QStringLiteral("</table>");
    info_->setHtml(html);

    threads_->setSortingEnabled(false);
    threads_->setRowCount(int(s->threadDetails.size()));
    int r = 0;
    auto numItem = [](double v, const QString& text) {
        auto* it = new QTableWidgetItem;
        it->setData(Qt::DisplayRole, text);
        it->setData(Qt::UserRole, v);
        it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return it;
    };
    for (const ThreadSample& t : s->threadDetails) {
        auto* tid = new QTableWidgetItem;
        tid->setData(Qt::DisplayRole, t.tid);
        threads_->setItem(r, 0, tid);
        threads_->setItem(r, 1, new QTableWidgetItem(t.comm));
        threads_->setItem(r, 2, new QTableWidgetItem(QString(QLatin1Char(t.state))));
        auto* cpu = new QTableWidgetItem;
        cpu->setData(Qt::DisplayRole, std::round(t.cpuPct * 10) / 10);
        cpu->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        threads_->setItem(r, 3, cpu);
        auto* rq = new QTableWidgetItem;
        rq->setData(Qt::DisplayRole, t.runDelayMsPs >= 0 ? std::round(t.runDelayMsPs * 10) / 10 : 0.0);
        rq->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        threads_->setItem(r, 4, rq);
        threads_->setItem(r, 5, numItem(t.nivcswPs, t.nivcswPs >= 0 ? fmt::count(t.nivcswPs) : QStringLiteral("–")));
        auto* lc = new QTableWidgetItem;
        lc->setData(Qt::DisplayRole, t.lastCpu);
        lc->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        threads_->setItem(r, 6, lc);
        ++r;
    }
    threads_->setSortingEnabled(true);
}

// ------------------------------------------------------------------ actions

void ProcessesTab::showContextMenu(const QPoint& pos)
{
    const QModelIndex idx = view_->indexAt(pos);
    if (!idx.isValid())
        return;
    const ProcSample* sp = model_->sample(proxy_->mapToSource(idx));
    if (!sp)
        return;
    const ProcSample s = *sp;   // the model may update while the menu is open

    QMenu menu(this);
    menu.addSection(QStringLiteral("%1 (%2)").arg(s.comm).arg(s.key.pid));
    menu.addAction(tr("Diagnose stutter for this process"), this, [this, s] { emit focusRequested(s.key.pid, s.comm); });
    menu.addSeparator();
    menu.addAction(tr("Terminate (SIGTERM)"), this, [this, s] { doSignal(s, SIGTERM); });
    menu.addAction(tr("Kill (SIGKILL)"), this, [this, s] { doSignal(s, SIGKILL); });
    menu.addAction(tr("Stop (SIGSTOP)"), this, [this, s] { doSignal(s, SIGSTOP); });
    menu.addAction(tr("Continue (SIGCONT)"), this, [this, s] { doSignal(s, SIGCONT); });
    menu.addSeparator();
    menu.addAction(tr("Change priority (nice)…"), this, [this, s] { doRenice(s); });
    menu.addAction(tr("I/O priority…"), this, [this, s] { doIonice(s); });
    menu.addAction(tr("CPU affinity…"), this, [this, s] { doAffinity(s); });
    menu.addSeparator();
    menu.addAction(tr("Copy command line"), this, [s] { QApplication::clipboard()->setText(s.cmdline); });
    menu.addAction(tr("Copy PID"), this, [s] { QApplication::clipboard()->setText(QString::number(s.key.pid)); });
    menu.exec(view_->viewport()->mapToGlobal(pos));
}

void ProcessesTab::doSignal(const ProcSample& s, int sig)
{
    if (sig == SIGKILL || sig == SIGTERM) {
        const bool kill = sig == SIGKILL;
        if (!Dialogs::question(this, kill ? tr("Kill process") : tr("End process"),
                               tr("Send %1 to %2 (pid %3)?%4")
                                   .arg(kill ? QStringLiteral("SIGKILL") : QStringLiteral("SIGTERM"), s.comm)
                                   .arg(s.key.pid)
                                   .arg(kill ? tr("\n\nSIGKILL can't be caught: unsaved work in that process is lost.") : QString()),
                               kill ? tr("Kill") : tr("End process"), tr("Cancel")))
            return;
    }
    report(ProcessActions::sendSignal(s.key, sig), QStringLiteral("signal"), s.key, {QString::number(sig)});
}

void ProcessesTab::doRenice(const ProcSample& s)
{
    int nice = s.nice;
    if (Dialogs::getInt(this, tr("Change priority"),
                        tr("Nice value for all threads of %1 (-20 = highest priority, 19 = lowest):").arg(s.comm), s.nice, -20, 19,
                        &nice))
        report(ProcessActions::renice(s.key, nice), QStringLiteral("renice"), s.key, {QString::number(nice)});
}

void ProcessesTab::doIonice(const ProcSample& s)
{
    ChromeDialog dlg(this, tr("I/O priority — %1").arg(s.comm));
    auto* form = new QFormLayout;
    dlg.body()->addLayout(form);
    auto* cls = new QComboBox;
    cls->addItem(tr("Best effort"), 2);
    cls->addItem(tr("Idle"), 3);
    cls->addItem(tr("Real-time (root)"), 1);
    cls->setToolTip(tr("Idle: only gets disk time when nothing else needs it. Real-time needs administrator rights."));
    auto* level = new QSpinBox;
    level->setRange(0, 7);
    level->setValue(4);
    form->addRow(tr("Class"), cls);
    form->addRow(tr("Level (0 = highest)"), level);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const int c = cls->currentData().toInt();
    report(ProcessActions::setIoPriority(s.key, c, level->value()), QStringLiteral("ionice"), s.key,
           {QString::number(c), QString::number(level->value())});
}

void ProcessesTab::doAffinity(const ProcSample& s)
{
    const int ncpu = lastFrame() ? lastFrame()->sys.ncpu : 1;
    const auto current = ProcessActions::affinity(s.key.pid);
    ChromeDialog dlg(this, tr("CPU affinity — %1").arg(s.comm));
    QVBoxLayout* v = dlg.body();
    v->addWidget(new QLabel(tr("Allowed CPUs:")));
    auto* grid = new QGridLayout;
    std::vector<QCheckBox*> boxes;
    for (int c = 0; c < ncpu; ++c) {
        auto* b = new QCheckBox(QString::number(c));
        b->setChecked(std::find(current.begin(), current.end(), c) != current.end());
        grid->addWidget(b, c / 8, c % 8);
        boxes.push_back(b);
    }
    v->addLayout(grid);
    auto* quick = new QHBoxLayout;
    auto* all = new QPushButton(tr("All"));
    auto* none = new QPushButton(tr("None"));
    quick->addWidget(all);
    quick->addWidget(none);
    quick->addStretch();
    v->addLayout(quick);
    connect(all, &QPushButton::clicked, &dlg, [&] { for (auto* b : boxes) b->setChecked(true); });
    connect(none, &QPushButton::clicked, &dlg, [&] { for (auto* b : boxes) b->setChecked(false); });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    std::vector<int> cpus;
    QStringList list;
    for (int c = 0; c < ncpu; ++c)
        if (boxes[size_t(c)]->isChecked()) {
            cpus.push_back(c);
            list << QString::number(c);
        }
    report(ProcessActions::setAffinity(s.key, cpus), QStringLiteral("affinity"), s.key, {list.join(QLatin1Char(','))});
}

void ProcessesTab::report(const ActionResult& r, const QString& action, const ProcKey& key, const QStringList& params)
{
    if (r.ok) {
        emit statusMessage(r.message);
        return;
    }
    if (r.permissionDenied) {
        const QStringList cmd = ProcessActions::privilegedCommand(action, key, params);
        if (!cmd.isEmpty() && Dialogs::question(this, tr("Permission denied"),
                                                tr("%1\n\nRetry as administrator?\n\n%2").arg(r.message, cmd.join(QLatin1Char(' '))),
                                                tr("Retry as administrator"), tr("Cancel"))) {
            runPrivileged(cmd);
        }
        return;
    }
    Dialogs::warning(this, tr("Action failed"), r.message);
}

void ProcessesTab::runPrivileged(const QStringList& command)
{
    auto* proc = new QProcess(this);
    connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus) {
        if (code == 0)
            emit statusMessage(tr("Done (as administrator)."));
        else if (code == 126 || code == 127)
            emit statusMessage(tr("Administrator authentication was cancelled or failed."));
        else
            Dialogs::warning(this, tr("Action failed"), QString::fromLocal8Bit(proc->readAllStandardError()));
        proc->deleteLater();
    });
    proc->start(QStringLiteral("/usr/bin/pkexec"), command);
}

} // namespace culprit
