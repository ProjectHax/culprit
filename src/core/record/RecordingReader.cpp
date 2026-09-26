// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/record/RecordingReader.h"

#include "core/record/Serialize.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

#include <unordered_map>

namespace culprit {

bool readRecording(const QString& path, Recording& out, QString* error)
{
    out = Recording{};
    out.path = path;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    std::unordered_map<quint64, size_t> hitchIndex;
    QHash<QString, size_t> openFinding;
    int bad = 0;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.isEmpty())
            continue;
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            ++bad;   // e.g. a truncated last line from a crash
            continue;
        }
        const QJsonObject o = doc.object();
        const QString t = o.value(QStringLiteral("t")).toString();
        if (t == QLatin1String("s")) {
            RecordingPoint p;
            p.wallMs = qint64(o.value(QStringLiteral("w")).toDouble());
            p.cpu = o.value(QStringLiteral("cpu")).toDouble();
            p.load = o.value(QStringLiteral("load")).toDouble();
            p.rq = o.value(QStringLiteral("rq")).toDouble();
            p.mem = o.value(QStringLiteral("mem")).toDouble();
            p.runnable = o.value(QStringLiteral("run")).toDouble(-1);
            p.blocked = o.value(QStringLiteral("blk")).toDouble(-1);
            p.tctl = o.value(QStringLiteral("tctl")).toDouble(-1000);
            p.pkgW = o.value(QStringLiteral("pkgW")).toDouble(-1);
            p.gpuT = o.value(QStringLiteral("gpuT")).toDouble(-1);
            p.gpuW = o.value(QStringLiteral("gpuW")).toDouble(-1);
            p.gpuU = o.value(QStringLiteral("gpuU")).toDouble(-1);
            p.latMs = o.value(QStringLiteral("lat")).toDouble(-1);
            p.disk = o.value(QStringLiteral("disk")).toDouble();
            p.swapin = o.value(QStringLiteral("swapin")).toDouble();
            p.majflt = o.value(QStringLiteral("majflt")).toDouble();
            for (const QJsonValue& v : o.value(QStringLiteral("top")).toArray()) {
                const QJsonArray a = v.toArray();
                if (a.size() >= 4)
                    p.top.push_back({a[0].toString(), a[1].toInt(), a[2].toDouble(), a[3].toDouble()});
            }
            out.points.push_back(std::move(p));
        } else if (t == QLatin1String("hitch")) {
            Hitch h = hitchFromJson(o);
            auto it = hitchIndex.find(h.id);
            if (o.value(QStringLiteral("update")).toBool() && it != hitchIndex.end()) {
                out.hitches[it->second] = h;
            } else {
                hitchIndex[h.id] = out.hitches.size();
                out.hitches.push_back(std::move(h));
            }
        } else if (t == QLatin1String("finding")) {
            const QString ev = o.value(QStringLiteral("event")).toString();
            Finding fi = findingFromJson(o);
            const qint64 w = qint64(o.value(QStringLiteral("w")).toDouble());
            if (ev == QLatin1String("raised")) {
                fi.firstSeenWallMs = w;
                fi.lastSeenWallMs = w;
                fi.active = true;
                openFinding[fi.id] = out.findings.size();
                out.findings.push_back(std::move(fi));
            } else if (openFinding.contains(fi.id)) {   // cleared, or still active when recording ended
                Finding& open = out.findings[openFinding.take(fi.id)];
                open.lastSeenWallMs = w;
                open.active = ev == QLatin1String("end");
                open.evidence = fi.evidence;   // last known evidence
                open.suspects = fi.suspects;
            }
        } else if (t == QLatin1String("header")) {
            if (out.header.isEmpty())
                out.header = o;
        }
    }
    if (!out.points.empty()) {
        out.startMs = out.points.front().wallMs;
        out.endMs = out.points.back().wallMs;
    } else if (!out.header.isEmpty()) {
        out.startMs = out.endMs = qint64(out.header.value(QStringLiteral("start")).toDouble());
    }
    if (out.points.empty() && out.hitches.empty()) {
        if (error)
            *error = bad ? QStringLiteral("No readable records (%1 malformed lines)").arg(bad) : QStringLiteral("Empty recording");
        return false;
    }
    return true;
}

} // namespace culprit
