// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/report/ReportExporter.h"

#include "core/Format.h"
#include "core/Version.h"
#include "core/record/Serialize.h"
#include "core/report/TextReport.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSysInfo>
#include <QTextStream>

#include <algorithm>
#include <map>

namespace culprit::Report {

namespace {

bool writeFile(const QString& path, const QByteArray& data, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    if (f.write(data) != data.size()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

QJsonObject systemInfo()
{
    return QJsonObject{{QStringLiteral("host"), QSysInfo::machineHostName()},
                       {QStringLiteral("kernel"), QSysInfo::kernelVersion()},
                       {QStringLiteral("os"), QSysInfo::prettyProductName()},
                       {QStringLiteral("generatedBy"), QStringLiteral("Culprit %1").arg(QString::fromLatin1(kVersion))},
                       {QStringLiteral("generated"), QDateTime::currentDateTime().toString(Qt::ISODate)}};
}

struct HitchStats {
    std::map<HitchClass, int> byClass;
    std::map<QString, int> bySuspect;
    double worst = 0;
    std::vector<double> periods;
};

HitchStats statsOf(const std::vector<Hitch>& hs)
{
    HitchStats st;
    for (const Hitch& h : hs) {
        st.byClass[h.cls]++;
        st.worst = std::max(st.worst, h.maxOvershootMs);
        if (!h.suspects.empty() && h.suspects.front().score >= 0.3) {
            QString name = h.suspects.front().label;   // group "sh x24" / "sh (123)" as "sh"
            for (const QString& sep : {QStringLiteral(" \u00d7"), QStringLiteral(" (")})
                if (const qsizetype i = name.indexOf(sep); i > 0)
                    name.truncate(i);
            st.bySuspect[name]++;
        }
        if (h.periodSec > 0 && (st.periods.empty() || std::abs(st.periods.back() - h.periodSec) > 0.1))
            st.periods.push_back(h.periodSec);
    }
    return st;
}

} // namespace

QString snapshotText(const Frame& f, const std::vector<Hitch>& hitches)
{
    QString s;
    QTextStream o(&s);
    o << "Culprit diagnostic report — " << QSysInfo::machineHostName() << ", " << QSysInfo::prettyProductName() << ", kernel "
      << QSysInfo::kernelVersion() << "\n";
    o << "Generated " << QDateTime::currentDateTime().toString(Qt::ISODate) << " by Culprit " << kVersion << "\n";
    o << frameText(f, hitches);
    return s;
}

QJsonObject snapshotJson(const Frame& f, const std::vector<Hitch>& hitches)
{
    QJsonArray findings, hs, procs, sensors, gpus;
    for (const Finding& fi : f.findings)
        findings.append(toJson(fi));
    for (const Hitch& h : hitches)
        hs.append(toJson(h));
    std::vector<const ProcSample*> top;
    for (const ProcSample& p : f.procs)
        top.push_back(&p);
    std::sort(top.begin(), top.end(), [](auto* a, auto* b) { return a->cpuPct > b->cpuPct; });
    for (size_t i = 0; i < top.size() && i < 30; ++i)
        procs.append(toJson(*top[i]));
    for (const SensorReading& r : f.sensors)
        sensors.append(QJsonObject{{QStringLiteral("key"), r.key}, {QStringLiteral("label"), r.label}, {QStringLiteral("value"), r.value}});
    for (const GpuSample& g : f.gpus)
        gpus.append(QJsonObject{{QStringLiteral("name"), g.name},
                                {QStringLiteral("tempC"), g.tempC},
                                {QStringLiteral("powerW"), g.powerW},
                                {QStringLiteral("util"), g.utilGpu},
                                {QStringLiteral("reasons"), QJsonArray::fromStringList(decodeGpuEventReasons(g.eventReasons))}});
    const SystemSample& y = f.sys;
    QJsonObject sys{{QStringLiteral("cpuBusyPct"), y.total.busy},
                    {QStringLiteral("load"), QJsonArray{y.load1, y.load5, y.load15}},
                    {QStringLiteral("cpus"), y.onlineCpus},
                    {QStringLiteral("runQueueWaiting"), y.runDelayTotalPct / 100.0},
                    {QStringLiteral("procsRunning"), int(y.procsRunning)},
                    {QStringLiteral("procsBlocked"), int(y.procsBlocked)},
                    {QStringLiteral("memUsedKb"), qint64(y.mem.usedKb)},
                    {QStringLiteral("memTotalKb"), qint64(y.mem.totalKb)},
                    {QStringLiteral("swapInPs"), y.vm.pswpin},
                    {QStringLiteral("cpuTempC"), f.thermal.hasCpuTemp() ? QJsonValue(f.thermal.cpuTempC) : QJsonValue()},
                    {QStringLiteral("packageW"), f.power.packageW},
                    {QStringLiteral("governor"), y.policy.governor},
                    {QStringLiteral("epp"), y.policy.epp}};
    return QJsonObject{{QStringLiteral("report"), QStringLiteral("snapshot")},
                       {QStringLiteral("system"), systemInfo()},
                       {QStringLiteral("state"), sys},
                       {QStringLiteral("findings"), findings},
                       {QStringLiteral("hitches"), hs},
                       {QStringLiteral("topProcesses"), procs},
                       {QStringLiteral("sensors"), sensors},
                       {QStringLiteral("gpus"), gpus}};
}

QString recordingText(const Recording& r)
{
    QString s;
    QTextStream o(&s);
    const double dur = double(r.endMs - r.startMs) / 1000.0;
    o << "Culprit recording report\n";
    o << "File: " << r.path << "\n";
    o << "Host: " << r.header.value(QStringLiteral("host")).toString() << " (" << r.header.value(QStringLiteral("os")).toString()
      << ", kernel " << r.header.value(QStringLiteral("kernel")).toString() << ")\n";
    o << "CPU: " << r.header.value(QStringLiteral("cpuModel")).toString() << ", GPU: " << r.header.value(QStringLiteral("gpu")).toString()
      << "\n";
    o << "Period: " << QDateTime::fromMSecsSinceEpoch(r.startMs).toString(Qt::ISODate) << " .. "
      << QDateTime::fromMSecsSinceEpoch(r.endMs).toString(QStringLiteral("HH:mm:ss")) << " (" << fmt::duration(dur) << ")\n";

    // Peaks
    const RecordingPoint* maxCpu = nullptr;
    const RecordingPoint* maxT = nullptr;
    const RecordingPoint* maxW = nullptr;
    const RecordingPoint* maxLoad = nullptr;
    const RecordingPoint* maxGpuT = nullptr;
    double avgCpu = 0;
    for (const RecordingPoint& p : r.points) {
        avgCpu += p.cpu;
        if (!maxCpu || p.cpu > maxCpu->cpu)
            maxCpu = &p;
        if (p.tctl > -100 && (!maxT || p.tctl > maxT->tctl))
            maxT = &p;
        if (!maxW || p.pkgW > maxW->pkgW)
            maxW = &p;
        if (!maxLoad || p.load > maxLoad->load)
            maxLoad = &p;
        if (p.gpuT >= 0 && (!maxGpuT || p.gpuT > maxGpuT->gpuT))
            maxGpuT = &p;
    }
    auto at = [](const RecordingPoint* p) { return p ? fmt::wallTime(p->wallMs).left(8) : QStringLiteral("–"); };
    auto topOf = [](const RecordingPoint* p) {
        QStringList l;
        if (p)
            for (size_t i = 0; i < p->top.size() && i < 3; ++i)
                l << QStringLiteral("%1 %2%").arg(p->top[i].comm).arg(p->top[i].cpu, 0, 'f', 0);
        return l.join(QStringLiteral(", "));
    };
    o << "\n== Peaks ==\n";
    if (!r.points.empty())
        o << "average CPU " << fmt::percent(avgCpu / double(r.points.size())) << "\n";
    if (maxCpu)
        o << "max CPU " << fmt::percent(maxCpu->cpu) << " at " << at(maxCpu) << " — " << topOf(maxCpu) << "\n";
    if (maxLoad)
        o << "max load " << QString::number(maxLoad->load, 'f', 2) << " at " << at(maxLoad) << "\n";
    if (maxT)
        o << "max CPU temperature " << fmt::celsius(maxT->tctl) << " at " << at(maxT) << " — " << topOf(maxT) << "\n";
    if (maxW && maxW->pkgW >= 0)
        o << "max CPU package power " << fmt::watts(maxW->pkgW) << " at " << at(maxW) << "\n";
    if (maxGpuT)
        o << "max GPU temperature " << fmt::celsius(maxGpuT->gpuT) << " at " << at(maxGpuT) << "\n";

    // Hitches
    const HitchStats st = statsOf(r.hitches);
    o << "\n== Stutter ==\n";
    o << r.hitches.size() << " hitch(es)";
    if (dur > 0)
        o << " (" << QString::number(double(r.hitches.size()) / (dur / 60.0), 'f', 1) << " per minute)";
    o << ", worst " << fmt::ms(st.worst) << "\n";
    for (const auto& [cls, n] : st.byClass)
        o << "  " << hitchClassName(cls) << ": " << n << "\n";
    if (!st.bySuspect.empty()) {
        std::vector<std::pair<int, QString>> sus;
        for (const auto& [name, n] : st.bySuspect)
            sus.push_back({n, name});
        std::sort(sus.rbegin(), sus.rend());
        o << "most frequent top suspects:\n";
        for (size_t i = 0; i < sus.size() && i < 8; ++i)
            o << "  " << sus[i].second << " — top suspect in " << sus[i].first << " hitch(es)\n";
    }
    for (double p : st.periods)
        o << "periodic pattern: every " << QString::number(p, 'f', 2) << " s\n";

    // Findings
    o << "\n== Findings (" << r.findings.size() << ") ==\n";
    std::vector<const Finding*> fs;
    for (const Finding& f : r.findings)
        fs.push_back(&f);
    std::sort(fs.begin(), fs.end(), [](auto* a, auto* b) {
        return a->severity != b->severity ? a->severity > b->severity : a->firstSeenWallMs < b->firstSeenWallMs;
    });
    for (const Finding* f : fs) {
        o << fmt::wallTime(f->firstSeenWallMs).left(8) << " for " << fmt::duration(double(f->lastSeenWallMs - f->firstSeenWallMs) / 1000.0)
          << "  ";
        o << findingText(*f);
    }

    // Worst hitches in detail
    if (!r.hitches.empty()) {
        std::vector<const Hitch*> hs;
        for (const Hitch& h : r.hitches)
            hs.push_back(&h);
        std::sort(hs.begin(), hs.end(), [](auto* a, auto* b) { return a->maxOvershootMs > b->maxOvershootMs; });
        o << "\n== Worst hitches ==\n";
        for (size_t i = 0; i < hs.size() && i < 15; ++i)
            o << hitchText(*hs[i]);
    }
    return s;
}

QJsonObject recordingJson(const Recording& r)
{
    const HitchStats st = statsOf(r.hitches);
    QJsonObject byClass;
    for (const auto& [cls, n] : st.byClass)
        byClass.insert(hitchClassKey(cls), n);
    QJsonArray findings, hitches;
    for (const Finding& f : r.findings)
        findings.append(toJson(f));
    for (const Hitch& h : r.hitches)
        hitches.append(toJson(h));
    return QJsonObject{{QStringLiteral("report"), QStringLiteral("recording")},
                       {QStringLiteral("file"), r.path},
                       {QStringLiteral("recordedOn"), r.header},
                       {QStringLiteral("system"), systemInfo()},
                       {QStringLiteral("start"), r.startMs},
                       {QStringLiteral("end"), r.endMs},
                       {QStringLiteral("samples"), qint64(r.points.size())},
                       {QStringLiteral("hitchCount"), qint64(r.hitches.size())},
                       {QStringLiteral("worstHitchMs"), st.worst},
                       {QStringLiteral("hitchesByClass"), byClass},
                       {QStringLiteral("findings"), findings},
                       {QStringLiteral("hitches"), hitches}};
}

bool saveSnapshot(const QString& path, const Frame& f, const std::vector<Hitch>& hitches, QString* error)
{
    if (path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive))
        return writeFile(path, QJsonDocument(snapshotJson(f, hitches)).toJson(QJsonDocument::Indented), error);
    return writeFile(path, snapshotText(f, hitches).toUtf8(), error);
}

bool saveRecording(const QString& path, const Recording& r, QString* error)
{
    if (path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive))
        return writeFile(path, QJsonDocument(recordingJson(r)).toJson(QJsonDocument::Indented), error);
    return writeFile(path, recordingText(r).toUtf8(), error);
}

} // namespace culprit::Report
