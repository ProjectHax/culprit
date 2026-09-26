// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/model/Frame.h"

#include <QJsonObject>
#include <QObject>
#include <QProcess>

namespace culprit {

// Talks to culprit-helper (started through pkexec, or directly when we are
// already root) over its stdin/stdout line protocol.
class HelperClient : public QObject {
    Q_OBJECT
public:
    explicit HelperClient(QObject* parent = nullptr);
    ~HelperClient() override;

    static QString helperPath();

    void start(int rqThresholdUs);
    void stop();
    bool running() const { return state_ == HelperStatus::State::Running || state_ == HelperStatus::State::Starting; }
    HelperStatus::State state() const { return state_; }
    QString message() const { return message_; }

    void requestWindow(quint64 id, int cpu, qint64 t0Ns, qint64 t1Ns);
    void setFocus(int pid);

signals:
    void stateChanged(culprit::HelperStatus::State state, const QString& message);
    void received(const QJsonObject& ev);

private:
    void send(const QByteArray& line);
    void onReadyRead();
    void onFinished(int code, QProcess::ExitStatus status);
    void setState(HelperStatus::State s, const QString& msg);

    QProcess* proc_ = nullptr;
    QString helperCopy_;   // AppImage: helper copied where root can read it
    QByteArray buf_;
    HelperStatus::State state_ = HelperStatus::State::Off;
    QString message_;
    int rqThresholdUs_ = 2000;
    bool stopping_ = false;
};

} // namespace culprit
