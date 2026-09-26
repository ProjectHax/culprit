// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/Format.h"

#include <QDateTime>

#include <cmath>

namespace culprit::fmt {

QString bytes(double b)
{
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int u = 0;
    while (std::fabs(b) >= 1024.0 && u < 4) {
        b /= 1024.0;
        ++u;
    }
    return u == 0 ? QStringLiteral("%1 B").arg(qint64(b))
                  : QStringLiteral("%1 %2").arg(b, 0, 'f', b < 10 ? 2 : 1).arg(QLatin1String(units[u]));
}

QString bytesPerSec(double bps)
{
    static const char* units[] = {"B/s", "kB/s", "MB/s", "GB/s"};
    int u = 0;
    while (std::fabs(bps) >= 1000.0 && u < 3) {
        bps /= 1000.0;
        ++u;
    }
    return QStringLiteral("%1 %2").arg(bps, 0, 'f', u == 0 ? 0 : 1).arg(QLatin1String(units[u]));
}

QString kb(uint64_t kb) { return bytes(double(kb) * 1024.0); }

QString percent(double pct, int decimals) { return QStringLiteral("%1%").arg(pct, 0, 'f', decimals); }

QString count(double v)
{
    if (std::fabs(v) >= 1e6)
        return QStringLiteral("%1M").arg(v / 1e6, 0, 'f', 1);
    if (std::fabs(v) >= 1e4)
        return QStringLiteral("%1k").arg(v / 1e3, 0, 'f', 0);
    if (std::fabs(v) >= 1e3)
        return QStringLiteral("%1k").arg(v / 1e3, 0, 'f', 1);
    if (std::fabs(v) >= 100 || v == std::floor(v))
        return QString::number(qint64(std::llround(v)));
    return QString::number(v, 'f', 1);
}

QString rate(double perSec) { return count(perSec) + QStringLiteral("/s"); }

QString ms(double ms)
{
    if (ms < 0)
        return QStringLiteral("–");
    if (ms < 1.0)
        return QStringLiteral("%1 µs").arg(ms * 1000.0, 0, 'f', 0);
    if (ms < 10.0)
        return QStringLiteral("%1 ms").arg(ms, 0, 'f', 2);
    if (ms < 1000.0)
        return QStringLiteral("%1 ms").arg(ms, 0, 'f', 1);
    return QStringLiteral("%1 s").arg(ms / 1000.0, 0, 'f', 2);
}

QString duration(double s)
{
    if (s < 60)
        return QStringLiteral("%1 s").arg(qint64(s));
    qint64 m = qint64(s / 60);
    if (m < 60)
        return QStringLiteral("%1m %2s").arg(m).arg(qint64(s) % 60, 2, 10, QLatin1Char('0'));
    qint64 h = m / 60;
    if (h < 48)
        return QStringLiteral("%1h %2m").arg(h).arg(m % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1d %2h").arg(h / 24).arg(h % 24);
}

QString celsius(double c)
{
    return c < -100 ? QStringLiteral("–") : QStringLiteral("%1 °C").arg(c, 0, 'f', 1);
}

QString watts(double w)
{
    return w < 0 ? QStringLiteral("–") : QStringLiteral("%1 W").arg(w, 0, 'f', w < 10 ? 1 : 0);
}

QString mhz(double mhz)
{
    if (mhz <= 0)
        return QStringLiteral("–");
    return mhz >= 1000 ? QStringLiteral("%1 GHz").arg(mhz / 1000.0, 0, 'f', 2)
                       : QStringLiteral("%1 MHz").arg(mhz, 0, 'f', 0);
}

QString wallTime(qint64 wallMs)
{
    return QDateTime::fromMSecsSinceEpoch(wallMs).toString(QStringLiteral("HH:mm:ss.zzz"));
}

QString procLabel(const QString& comm, int pid)
{
    return QStringLiteral("%1 (%2)").arg(comm).arg(pid);
}

} // namespace culprit::fmt
