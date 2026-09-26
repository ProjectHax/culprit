// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include <QString>

#include <cstdint>

namespace culprit::fmt {

QString bytes(double b);                 // "1.4 GiB"
QString bytesPerSec(double bps);         // "12.3 MB/s"
QString kb(uint64_t kb);
QString percent(double pct, int decimals = 1);
QString rate(double perSec);             // "12.4k/s"
QString count(double v);                 // "12.4k"
QString ms(double ms);                   // "3.2 ms" / "850 µs"
QString duration(double seconds);        // "1h 03m", "45 s"
QString celsius(double c);
QString watts(double w);
QString mhz(double mhz);
QString wallTime(qint64 wallMs);         // "14:03:22.415"
QString procLabel(const QString& comm, int pid);   // "firefox (4242)"

} // namespace culprit::fmt
