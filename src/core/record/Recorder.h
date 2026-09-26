// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/model/Frame.h"

#include <QFile>
#include <QHash>
#include <QSet>
#include <QString>

namespace culprit {

// Writes a recording: a JSON-lines file with a header, a compact summary line
// per frame, every hitch (and its deep-trace update) and finding transitions.
// Rotates to a new file after 64 MiB.
class Recorder {
public:
    ~Recorder() { close(); }

    bool open(const QString& dir, QString* error);
    void close();
    bool isOpen() const { return file_.isOpen(); }
    QString path() const { return file_.fileName(); }
    qint64 bytes() const { return bytes_; }
    int hitchCount() const { return hitches_; }

    void writeFrame(const Frame& f);
    void writeHitch(const Hitch& h, bool update = false);

    static QString defaultFileName();

private:
    void writeLine(const QByteArray& json);
    void writeHeader(const Frame* f);

    QFile file_;
    QString dir_;
    qint64 bytes_ = 0;
    int hitches_ = 0;
    bool headerWritten_ = false;
    QHash<QString, Finding> active_;
};

} // namespace culprit
