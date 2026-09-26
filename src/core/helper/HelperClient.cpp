// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/helper/HelperClient.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

#include <unistd.h>

namespace culprit {

HelperClient::HelperClient(QObject* parent) : QObject(parent) {}

HelperClient::~HelperClient()
{
    if (proc_) {
        stopping_ = true;
        proc_->write("quit\n");
        proc_->closeWriteChannel();
        if (!proc_->waitForFinished(1500))
            proc_->kill();   // root process: this only works if pkexec is still our child
    }
    if (!helperCopy_.isEmpty())
        QFile::remove(helperCopy_);
}

QString HelperClient::helperPath()
{
    // Next to the executable (build tree), ../libexec relative to it (installed
    // under any prefix), else the configured libexec location.
    const QString dir = QCoreApplication::applicationDirPath();
    for (const QString& p : {dir + QStringLiteral("/culprit-helper"), dir + QStringLiteral("/../libexec/culprit-helper")})
        if (QFileInfo::exists(p))
            return QFileInfo(p).canonicalFilePath();
    return QStringLiteral(CULPRIT_LIBEXECDIR "/culprit-helper");
}

void HelperClient::setState(HelperStatus::State s, const QString& msg)
{
    state_ = s;
    message_ = msg;
    emit stateChanged(s, msg);
}

void HelperClient::start(int rqThresholdUs)
{
    if (proc_)
        return;
    rqThresholdUs_ = rqThresholdUs;
    QString helper = helperPath();
    if (!QFileInfo(helper).isExecutable()) {
        setState(HelperStatus::State::Failed, tr("culprit-helper not found at %1").arg(helper));
        return;
    }
    if (qEnvironmentVariableIsSet("APPIMAGE") && geteuid() != 0) {
        // Root can't read the AppImage's FUSE mount; run a copy from the user's
        // runtime directory instead (removed again when the helper exits).
        const QString runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR", QStringLiteral("/tmp"));
        helperCopy_ = QStringLiteral("%1/culprit-helper-%2").arg(runtimeDir).arg(QCoreApplication::applicationPid());
        QFile::remove(helperCopy_);
        if (!QFile::copy(helper, helperCopy_)
            || !QFile::setPermissions(helperCopy_, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
            setState(HelperStatus::State::Failed, tr("Could not copy culprit-helper to %1").arg(runtimeDir));
            QFile::remove(helperCopy_);
            helperCopy_.clear();
            return;
        }
        helper = helperCopy_;
    }
    stopping_ = false;
    buf_.clear();
    proc_ = new QProcess(this);
    connect(proc_, &QProcess::readyReadStandardOutput, this, &HelperClient::onReadyRead);
    connect(proc_, &QProcess::finished, this, &HelperClient::onFinished);
    connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            setState(HelperStatus::State::Failed, tr("Could not start pkexec: %1").arg(proc_->errorString()));
    });
    proc_->setProcessChannelMode(QProcess::SeparateChannels);
    setState(HelperStatus::State::Starting, tr("Waiting for administrator authentication…"));
    if (geteuid() == 0)
        proc_->start(helper, {QStringLiteral("--proto=1")});
    else
        // pkexec needs an absolute path and clears the environment; the helper needs neither.
        proc_->start(QStringLiteral("/usr/bin/pkexec"), {helper, QStringLiteral("--proto=1")});
}

void HelperClient::stop()
{
    if (!proc_)
        return;
    stopping_ = true;
    send("quit\n");
    proc_->closeWriteChannel();   // EOF also makes the helper exit
}

void HelperClient::send(const QByteArray& line)
{
    if (proc_ && proc_->state() == QProcess::Running)
        proc_->write(line);
}

void HelperClient::requestWindow(quint64 id, int cpu, qint64 t0Ns, qint64 t1Ns)
{
    if (state_ == HelperStatus::State::Running)
        send(QStringLiteral("window %1 %2 %3 %4\n").arg(id).arg(cpu).arg(t0Ns).arg(t1Ns).toLatin1());
}

void HelperClient::setFocus(int pid)
{
    if (state_ == HelperStatus::State::Running)
        send(pid > 0 ? QStringLiteral("focus %1\n").arg(pid).toLatin1() : QByteArray("focus none\n"));
}

void HelperClient::onReadyRead()
{
    buf_ += proc_->readAllStandardOutput();
    qsizetype nl;
    while ((nl = buf_.indexOf('\n')) >= 0) {
        const QByteArray line = buf_.left(nl);
        buf_.remove(0, nl + 1);
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        const QJsonObject ev = doc.object();
        const QString t = ev.value(QStringLiteral("t")).toString();
        if (t == QLatin1String("hello")) {
            if (ev.value(QStringLiteral("proto")).toInt() != 1) {
                setState(HelperStatus::State::Failed, tr("Helper protocol mismatch"));
                stop();
                return;
            }
            setState(HelperStatus::State::Running, tr("Deep trace running (kernel %1)").arg(ev.value(QStringLiteral("kernel")).toString()));
            send(QStringLiteral("start sched,irq,softirq,reclaim,block,dstate,pio thresh_us=%1\n").arg(rqThresholdUs_).toLatin1());
            send("sysctl delayacct on\n");   // per-process block-I/O wait (restored on exit)
        } else if (t == QLatin1String("err") && state_ != HelperStatus::State::Running) {
            setState(HelperStatus::State::Failed, ev.value(QStringLiteral("msg")).toString());
        }
        emit received(ev);
    }
}

void HelperClient::onFinished(int code, QProcess::ExitStatus status)
{
    const QString errText = QString::fromLocal8Bit(proc_->readAllStandardError()).trimmed();
    proc_->deleteLater();
    proc_ = nullptr;
    if (!helperCopy_.isEmpty()) {
        QFile::remove(helperCopy_);
        helperCopy_.clear();
    }
    if (stopping_) {
        setState(HelperStatus::State::Off, QString());
        return;
    }
    QString msg;
    if (code == 126)
        msg = tr("Administrator authentication was cancelled.");
    else if (code == 127)
        msg = tr("Administrator authentication failed (or no polkit agent is running).");
    else if (status == QProcess::CrashExit)
        msg = tr("The helper crashed.");
    else
        msg = tr("The helper exited (code %1). %2").arg(code).arg(errText.left(300));
    setState(HelperStatus::State::Failed, msg);
}

} // namespace culprit
