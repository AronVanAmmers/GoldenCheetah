/*
 * Copyright (c) 2026 Aron van Ammers
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

//
// The command line entry point: argv in, text or JSON out, exit status.
// All the work is done by the shared core (CommandRunner), this file only
// adapts it to a terminal.
//

#include "CliMain.h"
#include "CliParser.h"
#include "HeadlessApp.h"
#include "HeadlessCommands.h"
#include "ResultFormat.h"
#include "RestServer.h"

#include "RideMetric.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <cstdio>
#include <cstdlib>

#ifdef Q_OS_WIN
#include <io.h>
#include <fcntl.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

namespace Headless {

static const char *program = "GoldenCheetah --cli";

bool
isCliInvocation(int argc, char **argv)
{
    if (argc < 1) return false;
    QString name = QFileInfo(QString::fromLocal8Bit(argv[0])).completeBaseName().toLower();
    if (name == "gc-cli" || name == "goldencheetah-cli" || name == "gccli") return true;
    for (int i = 1; i < argc; i++) if (qstrcmp(argv[i], "--cli") == 0) return true;
    return false;
}

static void
writeOut(const QByteArray &bytes)
{
    fwrite(bytes.constData(), 1, bytes.size(), stdout);
    fflush(stdout);
}

static void
writeErr(const QString &text)
{
    QByteArray b = text.toLocal8Bit();
    fwrite(b.constData(), 1, b.size(), stderr);
    fflush(stderr);
}

// the serve command belongs to the command line, not the core: it runs the
// REST adapter over the same core
static Command
serveCommand()
{
    Command serve;
    serve.spec.name = "serve";
    serve.spec.summary = "run the REST API server (all commands over HTTP)";
    serve.spec.description =
        "Serves every command under /v1 as JSON over HTTP until interrupted. Each\n"
        "request opens the athlete, runs and closes it again, exactly like a command\n"
        "line run. GET /v1/openapi.json describes the API.";
    serve.spec.scope = Scope::Global;
    serve.spec.params << ParamSpec("port", ParamType::Int, "TCP port").def(12022);
    serve.spec.params << ParamSpec("host", ParamType::String, "address to listen on").def("127.0.0.1");
    serve.spec.params << ParamSpec("token", ParamType::String, "require 'Authorization: Bearer TOKEN' (default: $GC_API_TOKEN)");
    serve.spec.params << ParamSpec("max-upload", ParamType::Int, "largest request body in MB").def(64);
    return serve;
}

static int
finish(int code)
{
    HeadlessApp::shutdown();
    fflush(stdout);
    fflush(stderr);

    // GoldenCheetah's globals were never designed to be torn down in order
    // (see terminate() in main.cpp), everything we own is saved by now
    _Exit(code);
    return code;
}

int
cliMain(int argc, char **argv)
{
    QStringList args;
    for (int i = 1; i < argc; i++) {
        if (qstrcmp(argv[i], "--cli") == 0) continue;
        args << QString::fromLocal8Bit(argv[i]);
    }

    CommandRegistry registry = commandRegistry();
    registry.add(serveCommand());

    CliParse parsed = CliParser::parse(args, registry);
    const GlobalOptions &g = parsed.global;
    bool json = g.format == "json", csv = g.format == "csv";

    if (!parsed.error.isEmpty()) {
        writeErr(QString("error: %1\n").arg(parsed.error));
        if (!parsed.helpTopic.isEmpty()) writeErr("\n" + CliParser::groupHelp(registry, parsed.helpTopic, program));
        else writeErr(QString("run '%1 --help' for usage\n").arg(program));
        return int(Status::Usage);
    }

    if (parsed.help || (parsed.command.isEmpty() && !parsed.version)) {
        QString text;
        if (parsed.helpTopic.isEmpty()) text = CliParser::usage(registry, program);
        else if (registry.find(parsed.helpTopic)) text = CliParser::commandHelp(registry.find(parsed.helpTopic)->spec, program);
        else text = CliParser::groupHelp(registry, parsed.helpTopic, program);
        writeOut(text.toLocal8Bit());
        return int(Status::Ok);
    }

    if (parsed.version) parsed.command = "version";

    // the core needs Qt (widgets for charts and metadata) and settings
    HeadlessApp::createApplication(argc, argv);

    // where are the athletes?
    QString home = g.home, athlete = g.athlete;
    if (!g.athleteDir.isEmpty()) {
        QFileInfo dir(g.athleteDir);
        home = dir.absolutePath();
        athlete = dir.fileName();
    }
    if (home.isEmpty()) home = HeadlessApp::defaultHome();
    home = QDir::cleanPath(QFileInfo(home).absoluteFilePath());

    HeadlessApp::Options appOptions;
    appOptions.python = !g.noPython;
    appOptions.verbose = g.verbose;
    HeadlessApp::setOptions(appOptions);
    if (QFileInfo(home).isDir()) {
        QString error;
        if (!HeadlessApp::initialise(home, appOptions, error)) {
            writeErr(QString("error: %1\n").arg(error));
            return finish(int(Status::Failed));
        }
    } else {
        // commands that don't need athletes still need the metric names
        RideMetricFactory::instance().initialize();
    }

    AthleteSession::Options sessionOptions;
    sessionOptions.lockWaitSeconds = g.lockWait;
    sessionOptions.force = g.force;

    if (parsed.command == "serve") {
        QJsonObject serveArgs = parsed.args;
        QString error = CommandRegistry::validate(serveCommand().spec, serveArgs);
        if (!error.isEmpty()) {
            writeErr(QString("error: %1\n").arg(error));
            return finish(int(Status::Usage));
        }
        if (!HeadlessApp::isInitialised()) {
            writeErr(QString("error: athletes folder '%1' does not exist\n").arg(home));
            return finish(int(Status::NotFound));
        }
        RestServer::Options options;
        options.host = serveArgs.value("host").toString();
        options.port = serveArgs.value("port").toInt();
        options.token = serveArgs.value("token").toString(qEnvironmentVariable("GC_API_TOKEN"));
        options.maxUploadBytes = qint64(serveArgs.value("max-upload").toInt()) * 1024 * 1024;
        options.home = home;
        options.session = sessionOptions;
        options.quiet = g.quiet;
        return finish(RestServer::run(options));
    }

    CommandRequest request;
    request.command = parsed.command;
    request.args = parsed.args;
    request.home = home;
    request.athlete = athlete;

    CommandRunner::Options runOptions;
    runOptions.session = sessionOptions;
    if (!g.quiet && !json && !csv && isatty(fileno(stderr))) {
        runOptions.progress = [](const QString &message) { writeErr(message + "\n"); };
    }

    CommandResult result = CommandRunner::run(commandRegistry(), request, runOptions);

    // binary output: a chart or an exported activity
    QString written;
    if (!result.payload.isEmpty()) {
        QString target = g.output.isEmpty() ? result.payloadName : g.output;
        if (target == "-") {
#ifdef Q_OS_WIN
            // images and exports are bytes, don't let the C runtime add \r
            _setmode(_fileno(stdout), _O_BINARY);
#endif
            writeOut(result.payload);
            return finish(int(result.status));
        }
        QFile out(target);
        if (!out.open(QFile::WriteOnly) || out.write(result.payload) != result.payload.size()) {
            writeErr(QString("error: can't write %1\n").arg(target));
            return finish(int(Status::Failed));
        }
        out.close();
        // as the platform writes paths (C:\... on Windows)
        written = QDir::toNativeSeparators(QFileInfo(target).absoluteFilePath());
        result.data.insert("output", written);
    }

    if (json) {
        writeOut(QJsonDocument(ResultFormat::envelope(request.command, result)).toJson(QJsonDocument::Indented));
    } else if (csv) {
        // only data on stdout, so it can go straight into a file or a pipe
        if (!result.ok() && result.status != Status::Partial) {
            writeErr(QString("error: %1\n").arg(result.error));
        } else {
            writeOut(ResultFormat::csv(result).toUtf8());
            if (result.status == Status::Partial) writeErr(QString("warning: %1\n").arg(result.error));
        }
        for (const QString &w : result.warnings) writeErr(QString("warning: %1\n").arg(w));
    } else {
        if (!result.ok() && result.status != Status::Partial) {
            // a per item report says what went wrong where
            if (!g.quiet && !result.text.isEmpty()) writeOut(result.text.toLocal8Bit());
            writeErr(QString("error: %1\n").arg(result.error));
        } else if (!g.quiet) {
            if (!written.isEmpty()) writeOut(QString("wrote %1 (%2 bytes)\n").arg(written).arg(result.payload.size()).toLocal8Bit());
            else writeOut(ResultFormat::text(result).toLocal8Bit());
            if (result.status == Status::Partial) writeErr(QString("warning: %1\n").arg(result.error));
        }
        for (const QString &w : result.warnings) writeErr(QString("warning: %1\n").arg(w));
    }
    return finish(int(result.status));
}

} // namespace Headless
