// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/record/Serialize.h"

#include <QJsonArray>

namespace culprit {

namespace {

QJsonArray strings(const QStringList& l)
{
    QJsonArray a;
    for (const QString& s : l)
        a.append(s);
    return a;
}

QStringList stringList(const QJsonValue& v)
{
    QStringList l;
    for (const QJsonValue& x : v.toArray())
        l << x.toString();
    return l;
}

} // namespace

QString hitchClassKey(HitchClass c)
{
    switch (c) {
    case HitchClass::RunQueue: return QStringLiteral("runqueue");
    case HitchClass::IrqKernel: return QStringLiteral("irq_kernel");
    case HitchClass::Reclaim: return QStringLiteral("reclaim");
    case HitchClass::Io: return QStringLiteral("io");
    case HitchClass::Thermal: return QStringLiteral("thermal");
    case HitchClass::Gpu: return QStringLiteral("gpu");
    case HitchClass::CgroupThrottle: return QStringLiteral("cgroup");
    case HitchClass::Global: return QStringLiteral("global");
    case HitchClass::Unknown: return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

HitchClass hitchClassFromKey(const QString& k)
{
    for (int c = 0; c <= int(HitchClass::Unknown); ++c)
        if (hitchClassKey(HitchClass(c)) == k)
            return HitchClass(c);
    return HitchClass::Unknown;
}

QJsonObject toJson(const Suspect& s)
{
    return QJsonObject{{QStringLiteral("kind"), s.kind},
                       {QStringLiteral("label"), s.label},
                       {QStringLiteral("pid"), s.pid},
                       {QStringLiteral("score"), s.score},
                       {QStringLiteral("evidence"), s.evidence}};
}

Suspect suspectFromJson(const QJsonObject& o)
{
    Suspect s;
    s.kind = o.value(QStringLiteral("kind")).toString();
    s.label = o.value(QStringLiteral("label")).toString();
    s.pid = o.value(QStringLiteral("pid")).toInt();
    s.score = o.value(QStringLiteral("score")).toDouble();
    s.evidence = o.value(QStringLiteral("evidence")).toString();
    return s;
}

QJsonObject toJson(const Finding& f)
{
    QJsonArray sus;
    for (const Suspect& s : f.suspects)
        sus.append(toJson(s));
    return QJsonObject{{QStringLiteral("id"), f.id},
                       {QStringLiteral("rule"), f.rule},
                       {QStringLiteral("severity"), severityName(f.severity).toLower()},
                       {QStringLiteral("title"), f.title},
                       {QStringLiteral("detail"), f.detail},
                       {QStringLiteral("advice"), f.advice},
                       {QStringLiteral("evidence"), strings(f.evidence)},
                       {QStringLiteral("suspects"), sus},
                       {QStringLiteral("firstSeen"), f.firstSeenWallMs},
                       {QStringLiteral("lastSeen"), f.lastSeenWallMs},
                       {QStringLiteral("active"), f.active}};
}

Finding findingFromJson(const QJsonObject& o)
{
    Finding f;
    f.id = o.value(QStringLiteral("id")).toString();
    f.rule = o.value(QStringLiteral("rule")).toString();
    const QString sev = o.value(QStringLiteral("severity")).toString();
    f.severity = sev == QLatin1String("critical") ? Severity::Critical : sev == QLatin1String("warning") ? Severity::Warning : Severity::Info;
    f.title = o.value(QStringLiteral("title")).toString();
    f.detail = o.value(QStringLiteral("detail")).toString();
    f.advice = o.value(QStringLiteral("advice")).toString();
    f.evidence = stringList(o.value(QStringLiteral("evidence")));
    for (const QJsonValue& v : o.value(QStringLiteral("suspects")).toArray())
        f.suspects.push_back(suspectFromJson(v.toObject()));
    f.firstSeenWallMs = qint64(o.value(QStringLiteral("firstSeen")).toDouble());
    f.lastSeenWallMs = qint64(o.value(QStringLiteral("lastSeen")).toDouble());
    f.active = o.value(QStringLiteral("active")).toBool(true);
    return f;
}

QJsonObject toJson(const Hitch& h)
{
    QJsonArray sus, cpus, blockers;
    for (const Suspect& s : h.suspects)
        sus.append(toJson(s));
    for (int c : h.cpus)
        cpus.append(c);
    for (const DeepBlocker& b : h.blockers)
        blockers.append(QJsonObject{{QStringLiteral("kind"), b.kind},
                                    {QStringLiteral("pid"), b.pid},
                                    {QStringLiteral("tid"), b.tid},
                                    {QStringLiteral("name"), b.name},
                                    {QStringLiteral("ms"), b.ms}});
    QJsonObject o{{QStringLiteral("id"), qint64(h.id)},
                  {QStringLiteral("wall"), h.wallMs},
                  {QStringLiteral("t0"), qint64(h.t0Ns)},
                  {QStringLiteral("t1"), qint64(h.t1Ns)},
                  {QStringLiteral("ms"), h.maxOvershootMs},
                  {QStringLiteral("rqShare"), h.runDelayShare},
                  {QStringLiteral("cpu"), h.worstCpu},
                  {QStringLiteral("cpus"), cpus},
                  {QStringLiteral("probes"), h.probesHit},
                  {QStringLiteral("global"), h.global},
                  {QStringLiteral("class"), hitchClassKey(h.cls)},
                  {QStringLiteral("summary"), h.summary},
                  {QStringLiteral("evidence"), strings(h.evidence)},
                  {QStringLiteral("suspects"), sus},
                  {QStringLiteral("period"), h.periodSec},
                  {QStringLiteral("self"), h.selfActivity}};
    if (h.deepResolved)
        o.insert(QStringLiteral("blockers"), blockers);
    return o;
}

Hitch hitchFromJson(const QJsonObject& o)
{
    Hitch h;
    h.id = quint64(o.value(QStringLiteral("id")).toDouble());
    h.wallMs = qint64(o.value(QStringLiteral("wall")).toDouble());
    h.t0Ns = qint64(o.value(QStringLiteral("t0")).toDouble());
    h.t1Ns = qint64(o.value(QStringLiteral("t1")).toDouble());
    h.maxOvershootMs = o.value(QStringLiteral("ms")).toDouble();
    h.runDelayShare = o.value(QStringLiteral("rqShare")).toDouble();
    h.worstCpu = o.value(QStringLiteral("cpu")).toInt(-1);
    for (const QJsonValue& v : o.value(QStringLiteral("cpus")).toArray())
        h.cpus.push_back(v.toInt());
    h.probesHit = o.value(QStringLiteral("probes")).toInt();
    h.global = o.value(QStringLiteral("global")).toBool();
    h.cls = hitchClassFromKey(o.value(QStringLiteral("class")).toString());
    h.summary = o.value(QStringLiteral("summary")).toString();
    h.evidence = stringList(o.value(QStringLiteral("evidence")));
    for (const QJsonValue& v : o.value(QStringLiteral("suspects")).toArray())
        h.suspects.push_back(suspectFromJson(v.toObject()));
    h.periodSec = o.value(QStringLiteral("period")).toDouble();
    h.selfActivity = o.value(QStringLiteral("self")).toBool();
    if (o.contains(QStringLiteral("blockers"))) {
        h.deepResolved = true;
        for (const QJsonValue& v : o.value(QStringLiteral("blockers")).toArray()) {
            const QJsonObject b = v.toObject();
            h.blockers.push_back({b.value(QStringLiteral("kind")).toString(), b.value(QStringLiteral("pid")).toInt(),
                                  b.value(QStringLiteral("tid")).toInt(), b.value(QStringLiteral("name")).toString(),
                                  b.value(QStringLiteral("ms")).toDouble()});
        }
    }
    return h;
}

QJsonObject toJson(const ProcSample& p)
{
    QJsonObject o{{QStringLiteral("pid"), p.key.pid},
                  {QStringLiteral("comm"), p.comm},
                  {QStringLiteral("user"), p.user},
                  {QStringLiteral("unit"), p.unit},
                  {QStringLiteral("cpu"), p.cpuPct},
                  {QStringLiteral("rssBytes"), qint64(p.rssBytes)},
                  {QStringLiteral("threads"), p.threads},
                  {QStringLiteral("state"), QString(QLatin1Char(p.state))},
                  {QStringLiteral("majfltPs"), p.majfltPs}};
    if (p.runDelayMsPs >= 0)
        o.insert(QStringLiteral("runDelayMsPs"), p.runDelayMsPs);
    if (p.nivcswPs >= 0)
        o.insert(QStringLiteral("preemptPs"), p.nivcswPs);
    if (p.estCpuW >= 0 || p.estGpuW >= 0)
        o.insert(QStringLiteral("estW"), std::max(0.0, p.estCpuW) + std::max(0.0, p.estGpuW));
    if (p.gpuPct >= 0)
        o.insert(QStringLiteral("gpuPct"), p.gpuPct);
    if (p.ioReadBps >= 0)
        o.insert(QStringLiteral("ioReadBps"), p.ioReadBps);
    if (p.ioWriteBps >= 0)
        o.insert(QStringLiteral("ioWriteBps"), p.ioWriteBps);
    return o;
}

} // namespace culprit
