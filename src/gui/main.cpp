// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#include "core/Engine.h"
#include "core/Format.h"
#include "core/Version.h"
#include "core/record/Recorder.h"
#include "core/record/RecordingReader.h"
#include "core/report/ReportExporter.h"
#include "core/report/TextReport.h"
#include "gui/MainWindow.h"
#include "gui/ThemeManager.h"
#include "gui/widgets/HeaderFit.h"

#include <QApplication>
#include <QIcon>
#include <QStandardPaths>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QTimer>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {

bool hasArg(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], name) == 0)
            return true;
    return false;
}

void setAppInfo()
{
    QCoreApplication::setApplicationName(QStringLiteral("culprit"));
    QCoreApplication::setOrganizationName(QStringLiteral("culprit"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(culprit::kVersion));
}

void addCommonOptions(QCommandLineParser& p)
{
    p.setApplicationDescription(QStringLiteral(
        "Culprit — finds the culprit behind high load, heat and micro-stutters on Linux.\n"
        "Copyright (C) 2026 ProjectHax LLC. License GPLv3+: GNU GPL version 3 or later."));
    p.addHelpOption();
    p.addVersionOption();
    p.addOption({QStringLiteral("snapshot"), QStringLiteral("Sample for a few seconds, print a text report and exit.")});
    p.addOption({QStringLiteral("seconds"), QStringLiteral("How long --snapshot samples (default 3)."), QStringLiteral("n"), QStringLiteral("3")});
    p.addOption({QStringLiteral("record"), QStringLiteral("Headless: record hitches, findings and 1 Hz summaries until stopped (SIGINT/SIGTERM).")});
    p.addOption({QStringLiteral("out"), QStringLiteral("Directory for --record (default ~/.local/share/culprit/recordings)."), QStringLiteral("dir")});
    p.addOption({QStringLiteral("duration"), QStringLiteral("Stop --record after this many seconds."), QStringLiteral("seconds")});
    p.addOption({QStringLiteral("deep"), QStringLiteral("With --record: also run the root tracing helper (via pkexec unless already root).")});
    p.addOption({QStringLiteral("report"), QStringLiteral("Print a report for a recording file and exit."), QStringLiteral("recording.jsonl")});
    p.addOption({QStringLiteral("open"), QStringLiteral("Open a recording in the Recordings tab."), QStringLiteral("recording.jsonl")});
    p.addOption({QStringLiteral("json"), QStringLiteral("With --report or --snapshot: output JSON instead of text.")});
    p.addOption({QStringLiteral("probe-mode"), QStringLiteral("Stutter probes: off, floating, percpu or rt."), QStringLiteral("mode")});
    p.addOption({QStringLiteral("grab"), QStringLiteral("Save a PNG screenshot of the window after --seconds, then exit."), QStringLiteral("file.png")});
    p.addOption({QStringLiteral("tab"), QStringLiteral("Tab to show at startup (overview, processes, thermals, load, stutter, recordings)."), QStringLiteral("name")});
}

void applyProbeOption(const QCommandLineParser& parser, culprit::Settings& s)
{
    const QString m = parser.value(QStringLiteral("probe-mode")).toLower();
    if (m == QLatin1String("off"))
        s.probeMode = culprit::ProbeMode::Off;
    else if (m == QLatin1String("floating"))
        s.probeMode = culprit::ProbeMode::Floating;
    else if (m == QLatin1String("percpu"))
        s.probeMode = culprit::ProbeMode::PerCpu;
    else if (m == QLatin1String("rt"))
        s.probeMode = culprit::ProbeMode::Realtime;
}

// Headless: sample for N seconds, print the last frame as text.
int runSnapshot(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    setAppInfo();
    QCommandLineParser parser;
    addCommonOptions(parser);
    parser.process(app);
    const int seconds = std::max(2, parser.value(QStringLiteral("seconds")).toInt());

    culprit::Settings settings = culprit::Settings::load();
    applyProbeOption(parser, settings);
    auto host = std::make_unique<culprit::EngineHost>(settings);
    culprit::FramePtr last;
    std::vector<culprit::Hitch> hitches;
    QObject::connect(host->engine(), &culprit::Engine::frameReady, &app,
                     [&](const culprit::FramePtr& f) { last = f; }, Qt::QueuedConnection);
    QObject::connect(host->engine(), &culprit::Engine::hitchDetected, &app,
                     [&](const culprit::Hitch& h) { hitches.push_back(h); }, Qt::QueuedConnection);
    QTimer::singleShot(seconds * 1000 + 200, &app, [&] {
        host->shutdown();
        if (last) {
            if (parser.isSet(QStringLiteral("json")))
                std::fputs(QJsonDocument(culprit::Report::snapshotJson(*last, hitches)).toJson().constData(), stdout);
            else
                std::fputs(culprit::frameText(*last, hitches).toUtf8().constData(), stdout);
        }
        app.quit();
    });
    host->start();
    return app.exec();
}

std::atomic<int> gStopSignal{0};

// Headless recorder (also what the systemd user unit runs).
int runRecorder(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    setAppInfo();
    QCommandLineParser parser;
    addCommonOptions(parser);
    parser.process(app);

    culprit::Settings settings = culprit::Settings::load();
    applyProbeOption(parser, settings);
    const QString dir = parser.isSet(QStringLiteral("out")) ? parser.value(QStringLiteral("out")) : settings.effectiveRecordingsDir();
    culprit::Recorder rec;
    QString err;
    if (!rec.open(dir, &err)) {
        std::fprintf(stderr, "culprit: cannot record to %s: %s\n", qPrintable(dir), qPrintable(err));
        return 1;
    }
    std::fprintf(stderr, "culprit: recording to %s\n", qPrintable(rec.path()));

    auto host = std::make_unique<culprit::EngineHost>(settings);
    culprit::Engine* engine = host->engine();
    QObject::connect(engine, &culprit::Engine::frameReady, &app, [&](const culprit::FramePtr& f) { rec.writeFrame(*f); }, Qt::QueuedConnection);
    QObject::connect(engine, &culprit::Engine::hitchDetected, &app, [&](const culprit::Hitch& h) { rec.writeHitch(h); }, Qt::QueuedConnection);
    QObject::connect(engine, &culprit::Engine::hitchUpdated, &app, [&](const culprit::Hitch& h) { rec.writeHitch(h, true); }, Qt::QueuedConnection);

    for (int sig : {SIGINT, SIGTERM, SIGHUP})
        std::signal(sig, [](int s) { gStopSignal = s; });
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        if (gStopSignal)
            app.quit();
    });
    poll.start(200);
    if (parser.isSet(QStringLiteral("duration")))
        QTimer::singleShot(std::max(1, parser.value(QStringLiteral("duration")).toInt()) * 1000, &app, &QCoreApplication::quit);

    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    host->start();
    if (parser.isSet(QStringLiteral("deep")))
        engine->setDeepTrace(true);
    const int rc = app.exec();
    host->shutdown();
    const QString path = rec.path();
    const int hitches = rec.hitchCount();
    rec.close();
    std::fprintf(stderr, "culprit: recorded %s, %d hitch(es) -> %s\n",
                 qPrintable(culprit::fmt::duration(double(QDateTime::currentMSecsSinceEpoch() - started) / 1000.0)), hitches,
                 qPrintable(path));
    return rc;
}

// Report for an existing recording.
int runReport(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    setAppInfo();
    QCommandLineParser parser;
    addCommonOptions(parser);
    parser.process(app);
    culprit::Recording r;
    QString err;
    if (!culprit::readRecording(parser.value(QStringLiteral("report")), r, &err)) {
        std::fprintf(stderr, "culprit: %s\n", qPrintable(err));
        return 1;
    }
    if (parser.isSet(QStringLiteral("json")))
        std::fputs(QJsonDocument(culprit::Report::recordingJson(r)).toJson().constData(), stdout);
    else
        std::fputs(culprit::Report::recordingText(r).toUtf8().constData(), stdout);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (hasArg(argc, argv, "--version") || hasArg(argc, argv, "-v")) {
        std::printf("culprit %s\n"
                    "Copyright (C) 2026 ProjectHax LLC\n"
                    "License GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>.\n"
                    "This is free software: you are free to change and redistribute it.\n"
                    "There is NO WARRANTY, to the extent permitted by law.\n",
                    culprit::kVersion);
        return 0;
    }
    if (hasArg(argc, argv, "--snapshot"))
        return runSnapshot(argc, argv);
    if (hasArg(argc, argv, "--record"))
        return runRecorder(argc, argv);
    if (hasArg(argc, argv, "--report"))
        return runReport(argc, argv);

    // Qt registers the app ID with xdg-desktop-portal while QApplication is being
    // constructed; set later, the registration is deferred until after Qt has
    // already talked to the portal and gets refused ("Connection already
    // associated with an application ID"). The portal also rejects IDs without an
    // installed desktop file, so only claim it when culprit.desktop is installed.
    if (!QStandardPaths::locate(QStandardPaths::ApplicationsLocation, QStringLiteral("culprit.desktop")).isEmpty())
        QGuiApplication::setDesktopFileName(QStringLiteral("culprit"));

    QApplication app(argc, argv);
    setAppInfo();
    QApplication::setApplicationDisplayName(QString::fromLatin1(culprit::kAppName));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("culprit"), QIcon(QStringLiteral(":/culprit.svg"))));
    QCommandLineParser parser;
    addCommonOptions(parser);
    parser.process(app);

    culprit::ThemeManager::instance().setMode(culprit::Settings::load().themeMode);
    culprit::HeaderFitter::install();
    culprit::MainWindow w;
    if (parser.isSet(QStringLiteral("tab")))
        w.showTab(parser.value(QStringLiteral("tab")));
    if (parser.isSet(QStringLiteral("open")))
        w.openRecording(parser.value(QStringLiteral("open")));
    w.show();

    if (parser.isSet(QStringLiteral("grab"))) {
        const QString file = parser.value(QStringLiteral("grab"));
        const int seconds = std::max(2, parser.value(QStringLiteral("seconds")).toInt());
        QTimer::singleShot(seconds * 1000, &app, [&w, file] {
            w.grab().save(file);
            QApplication::quit();
        });
    }
    return app.exec();
}
