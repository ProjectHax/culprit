// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "gui/widgets/HitchDetail.h"

#include "core/Format.h"
#include "gui/Theme.h"

#include <QObject>

#include <algorithm>

namespace culprit {

QColor hitchColor(HitchClass c)
{
    switch (c) {
    case HitchClass::RunQueue: return theme::orange();
    case HitchClass::IrqKernel: return theme::purple();
    case HitchClass::Reclaim: return theme::red();
    case HitchClass::Io: return QColor(0x06, 0xb6, 0xd4);
    case HitchClass::Thermal: return QColor(0xdc, 0x26, 0x26);
    case HitchClass::Gpu: return theme::green();
    case HitchClass::CgroupThrottle: return QColor(0xec, 0x48, 0x99);
    case HitchClass::Global: return QColor(0x7c, 0x3a, 0xed);
    case HitchClass::Unknown: return QColor(0x88, 0x88, 0x88);
    }
    return {};
}

QString hitchHtml(const Hitch& h, const QPalette& pal)
{
    const QString dim = theme::dimText(pal).name(QColor::HexArgb);
    QString s = QStringLiteral("<h3 style='margin:0 0 4px 0'><span style='color:%1'>■</span> %2</h3>")
                    .arg(hitchColor(h.cls).name(), h.summary.toHtmlEscaped());
    QStringList cpus;
    for (int c : h.cpus)
        cpus << QString::number(c);
    s += QStringLiteral("<div style='color:%1'>%2 · %3 · worst %4 on CPU %5 · CPUs %6%7%8</div>")
             .arg(dim, fmt::wallTime(h.wallMs), hitchClassName(h.cls), fmt::ms(h.maxOvershootMs))
             .arg(h.worstCpu)
             .arg(cpus.join(QLatin1Char(',')))
             .arg(h.periodSec > 0 ? QObject::tr(" · <b>repeats every %1 s</b>").arg(h.periodSec, 0, 'f', 2) : QString())
             .arg(h.global ? QObject::tr(" · <b>system-wide</b>") : QString());
    s += QStringLiteral("<p>%1</p>").arg(hitchClassDescription(h.cls).toHtmlEscaped());

    // Where the delay went
    const double rq = h.maxOvershootMs * h.runDelayShare;
    s += QObject::tr("<p style='margin-bottom:2px'><b>Where the %1 went</b></p>").arg(fmt::ms(h.maxOvershootMs));
    s += QStringLiteral("<table cellspacing=0 cellpadding=1><tr><td>%1</td><td style='padding-left:8px'>%2</td></tr>"
                        "<tr><td>%3</td><td style='padding-left:8px'>%4</td></tr></table>")
             .arg(QObject::tr("waiting runnable (CPU busy with another task)"), fmt::ms(rq),
                  QObject::tr("before being woken (IRQ / kernel / firmware)"), fmt::ms(h.maxOvershootMs - rq));

    if (h.deepResolved) {
        s += QObject::tr("<p style='margin-bottom:2px'><b>CPU %1 during the stall</b> (deep trace)</p><table cellspacing=0 cellpadding=2>")
                 .arg(h.worstCpu);
        double total = 0;
        for (const DeepBlocker& b : h.blockers)
            total += b.ms;
        for (const DeepBlocker& b : h.blockers) {
            const QString who = b.kind == QLatin1String("task")
                                    ? QStringLiteral("<a href='pid:%1'>%2</a> <span style='color:%3'>(pid %1, tid %4)</span>").arg(b.pid).arg(b.name.toHtmlEscaped(), dim).arg(b.tid)
                                    : QStringLiteral("%1 <span style='color:%2'>(%3)</span>").arg(b.name.toHtmlEscaped(), dim, b.kind);
            const int bar = total > 0 ? int(b.ms / total * 120) : 0;
            s += QStringLiteral("<tr><td>%1</td><td style='padding-left:8px'>%2</td><td><table cellspacing=0 cellpadding=0><tr>"
                                "<td width=%3 height=9 bgcolor='%4'></td></tr></table></td></tr>")
                     .arg(who, fmt::ms(b.ms))
                     .arg(std::max(2, bar))
                     .arg(hitchColor(h.cls).name());
        }
        s += QStringLiteral("</table>");
    }

    if (!h.suspects.empty()) {
        s += QObject::tr("<p style='margin-bottom:2px'><b>Suspects</b></p><table cellspacing=0 cellpadding=2>");
        for (const Suspect& su : h.suspects) {
            const QString label = su.pid > 0 && su.kind == QLatin1String("process")
                                      ? QStringLiteral("<a href='pid:%1'>%2</a>").arg(su.pid).arg(su.label.toHtmlEscaped())
                                      : su.label.toHtmlEscaped();
            s += QStringLiteral("<tr><td><table cellspacing=0 cellpadding=0><tr><td width=%1 height=9 bgcolor='%2'></td></tr></table></td>"
                                "<td style='padding-left:6px'>%3</td><td style='padding-left:10px;color:%4'>%5</td></tr>")
                     .arg(std::max(3, int(su.score * 60)))
                     .arg(hitchColor(h.cls).name(), label, dim, su.evidence.toHtmlEscaped());
        }
        s += QStringLiteral("</table>");
    }
    if (!h.evidence.isEmpty()) {
        s += QObject::tr("<p style='margin-bottom:2px'><b>Evidence</b></p><ul style='margin-top:0'>");
        for (const QString& e : h.evidence)
            s += QStringLiteral("<li>%1</li>").arg(e.toHtmlEscaped());
        s += QStringLiteral("</ul>");
    }
    if (!h.deepResolved)
        s += QObject::tr("<p style='color:%1'>Deep trace shows exactly what held the CPU.</p>").arg(dim);
    return s;
}

} // namespace culprit
