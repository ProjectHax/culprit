// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/record/Recorder.h"

#include "common/fs/File.h"
#include "core/Version.h"
#include "core/record/Serialize.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSysInfo>

#include <algorithm>

namespace culprit {

namespace {
constexpr qint64 kRotateBytes = 64ll * 1024 * 1024;

QString cpuModel()
{
    std::string info;
    if (!readFile(SysPaths::proc_("cpuinfo"), info))
        return {};
    const auto p = info.find("model name");
    if (p == std::string::npos)
        return {};
    const auto colon = info.find(':', p);
    const auto nl = info.find('\n', p);
    return QString::fromStdString(info.substr(colon + 2, nl - colon - 2));
}
} // namespace

QString Recorder::defaultFileName()
{
    return QStringLiteral("culprit-%1.jsonl").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
}

bool Recorder::open(const QString& dir, QString* error)
{
    close();
    dir_ = dir;
    if (!QDir().mkpath(dir)) {
        if (error)
            *error = QStringLiteral("Cannot create %1").arg(dir);
        return false;
    }
    file_.setFileName(QDir(dir).filePath(defaultFileName()));
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (error)
            *error = file_.errorString();
        return false;
    }
    bytes_ = 0;
    hitches_ = 0;
    headerWritten_ = false;
    active_.clear();
    return true;
}

void Recorder::close()
{
    if (!file_.isOpen())
        return;
    // Mark findings still active at the end.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const Finding& f : std::as_const(active_)) {
        QJsonObject o = toJson(f);
        o.insert(QStringLiteral("t"), QStringLiteral("finding"));
        o.insert(QStringLiteral("event"), QStringLiteral("end"));
        o.insert(QStringLiteral("w"), now);
        writeLine(QJsonDocument(o).toJson(QJsonDocument::Compact));
    }
    active_.clear();
    writeLine(QJsonDocument(QJsonObject{{QStringLiteral("t"), QStringLiteral("footer")}, {QStringLiteral("w"), now}})
                  .toJson(QJsonDocument::Compact));
    file_.close();
}

void Recorder::writeLine(const QByteArray& json)
{
    if (!file_.isOpen())
        return;
    file_.write(json);
    file_.write("\n", 1);
    file_.flush();
    bytes_ += json.size() + 1;
}

void Recorder::writeHeader(const Frame* f)
{
    QJsonObject h{{QStringLiteral("t"), QStringLiteral("header")},
                  {QStringLiteral("version"), 1},
                  {QStringLiteral("app"), QStringLiteral("culprit")},
                  {QStringLiteral("appVersion"), QString::fromLatin1(kVersion)},
                  {QStringLiteral("host"), QSysInfo::machineHostName()},
                  {QStringLiteral("kernel"), QSysInfo::kernelVersion()},
                  {QStringLiteral("os"), QSysInfo::prettyProductName()},
                  {QStringLiteral("cpuModel"), cpuModel()},
                  {QStringLiteral("start"), QDateTime::currentMSecsSinceEpoch()}};
    if (f) {
        h.insert(QStringLiteral("ncpu"), f->sys.onlineCpus);
        h.insert(QStringLiteral("memKb"), qint64(f->sys.mem.totalKb));
        h.insert(QStringLiteral("tjmax"), f->thermal.tjmaxC);
        if (!f->gpus.empty())
            h.insert(QStringLiteral("gpu"), f->gpus.front().name);
    }
    writeLine(QJsonDocument(h).toJson(QJsonDocument::Compact));
    headerWritten_ = true;
}

void Recorder::writeFrame(const Frame& f)
{
    if (!file_.isOpen())
        return;
    if (bytes_ > kRotateBytes) {
        const QString dir = dir_;
        open(dir, nullptr);
    }
    if (!headerWritten_)
        writeHeader(&f);

    // Compact per-second summary.
    const SystemSample& s = f.sys;
    QJsonObject o{{QStringLiteral("t"), QStringLiteral("s")},
                  {QStringLiteral("w"), f.wallMs},
                  {QStringLiteral("cpu"), qRound(s.total.busy * 10) / 10.0},
                  {QStringLiteral("load"), qRound(s.load1 * 100) / 100.0},
                  {QStringLiteral("rq"), qRound(s.runDelayTotalPct) / 100.0},
                  {QStringLiteral("mem"), s.mem.totalKb ? qRound(double(s.mem.usedKb) * 1000.0 / double(s.mem.totalKb)) / 10.0 : 0.0}};
    if (s.avgRunnable >= 0) {
        o.insert(QStringLiteral("run"), qRound(s.avgRunnable * 10) / 10.0);
        o.insert(QStringLiteral("blk"), qRound(s.avgBlocked * 10) / 10.0);
    }
    if (s.vm.pswpin > 0)
        o.insert(QStringLiteral("swapin"), qRound(s.vm.pswpin));
    if (s.vm.pgmajfault > 0)
        o.insert(QStringLiteral("majflt"), qRound(s.vm.pgmajfault));
    if (f.thermal.hasCpuTemp())
        o.insert(QStringLiteral("tctl"), qRound(f.thermal.cpuTempC * 10) / 10.0);
    if (f.power.packageW >= 0)
        o.insert(QStringLiteral("pkgW"), qRound(f.power.packageW * 10) / 10.0);
    if (!f.gpus.empty()) {
        const GpuSample& g = f.gpus.front();
        o.insert(QStringLiteral("gpuT"), g.tempC);
        o.insert(QStringLiteral("gpuW"), qRound(g.powerW * 10) / 10.0);
        o.insert(QStringLiteral("gpuU"), g.utilGpu);
        if (g.eventReasons & ~GpuReason::Idle)
            o.insert(QStringLiteral("gpuR"), qint64(g.eventReasons));
    }
    if (f.stutter.probes > 0)
        o.insert(QStringLiteral("lat"), qRound(f.stutter.worstMs * 100) / 100.0);
    double disk = 0;
    for (const DiskSample& d : s.disks)
        disk = std::max(disk, d.utilPct);
    o.insert(QStringLiteral("disk"), qRound(disk));
    // Top processes by CPU.
    std::vector<const ProcSample*> top;
    for (const ProcSample& p : f.procs)
        if (p.cpuPct >= 1)
            top.push_back(&p);
    std::sort(top.begin(), top.end(), [](auto* a, auto* b) { return a->cpuPct > b->cpuPct; });
    QJsonArray tops;
    for (size_t i = 0; i < top.size() && i < 5; ++i)
        tops.append(QJsonArray{top[i]->comm, top[i]->key.pid, qRound(top[i]->cpuPct * 10) / 10.0,
                               qRound((std::max(0.0, top[i]->estCpuW) + std::max(0.0, top[i]->estGpuW)) * 10) / 10.0});
    o.insert(QStringLiteral("top"), tops);
    writeLine(QJsonDocument(o).toJson(QJsonDocument::Compact));

    // Finding transitions.
    QSet<QString> now;
    for (const Finding& fi : f.findings) {
        if (!fi.active)
            continue;
        now.insert(fi.id);
        if (!active_.contains(fi.id)) {
            QJsonObject fo = toJson(fi);
            fo.insert(QStringLiteral("t"), QStringLiteral("finding"));
            fo.insert(QStringLiteral("event"), QStringLiteral("raised"));
            fo.insert(QStringLiteral("w"), f.wallMs);
            writeLine(QJsonDocument(fo).toJson(QJsonDocument::Compact));
        }
        active_.insert(fi.id, fi);
    }
    for (auto it = active_.begin(); it != active_.end();) {
        if (!now.contains(it.key())) {
            QJsonObject fo = toJson(it.value());
            fo.insert(QStringLiteral("t"), QStringLiteral("finding"));
            fo.insert(QStringLiteral("event"), QStringLiteral("cleared"));
            fo.insert(QStringLiteral("w"), f.wallMs);
            writeLine(QJsonDocument(fo).toJson(QJsonDocument::Compact));
            it = active_.erase(it);
        } else {
            ++it;
        }
    }
}

void Recorder::writeHitch(const Hitch& h, bool update)
{
    if (!file_.isOpen())
        return;
    if (!headerWritten_)
        writeHeader(nullptr);
    QJsonObject o = toJson(h);
    o.insert(QStringLiteral("t"), QStringLiteral("hitch"));
    if (update)
        o.insert(QStringLiteral("update"), true);
    else
        ++hitches_;
    writeLine(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

} // namespace culprit
