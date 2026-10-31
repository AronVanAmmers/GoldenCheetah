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

#include "SessionServer.h"
#include "AthleteLock.h"
#include "HeadlessCommands.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QTimer>
#include <QtEndian>
#include <csignal>
#include <cstdio>
#include <cstdlib>

namespace Headless {

static volatile std::sig_atomic_t stopRequested = 0;
static void onSignal(int)
{
    // a second ctrl-c when the first one didn't get us out
    if (stopRequested) std::_Exit(130);
    stopRequested = 1;
}

static void
say(const QString &text)
{
    QByteArray b = text.toLocal8Bit();
    fwrite(b.constData(), 1, b.size(), stderr);
    fflush(stderr);
}

// the canonical athlete folder, as the session has it (the path as given
// when it doesn't exist: the session then fails to open it, and says so)
static QString
athleteFolder(const QString &home, const QString &athlete)
{
    QString path = QDir::cleanPath(QDir(home).absoluteFilePath(athlete));
    QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? path : canonical;
}

// where something listens for this athlete folder: a socket next to the
// athlete lock, which only this user can reach, or a named pipe on Windows
static QString
localName(const QString &folder, const QString &suffix)
{
    QString path = AthleteLock::runtimeFilePath(folder, suffix);
#ifndef Q_OS_WIN
    // a socket's path is limited to about 100 bytes (104 on macOS, whose
    // temporary folders are long): longer ones go in Qt's temporary folder
    if (QFile::encodeName(path).size() < 100) return path;
#endif
    return "GoldenCheetah-" + QFileInfo(path).fileName().replace('.', '-');
}

static QString infoPath(const QString &folder) { return AthleteLock::runtimeFilePath(folder, ".session"); }
static QString logPath(const QString &folder) { return AthleteLock::runtimeFilePath(folder, ".session.log"); }

//
// messages are JSON objects, each after its length (4 bytes, big endian)
//

static void
writeMessage(QLocalSocket *socket, const QJsonObject &message)
{
    QByteArray body = QJsonDocument(message).toJson(QJsonDocument::Compact);
    QByteArray size(4, '\0');
    qToBigEndian<quint32>(quint32(body.size()), size.data());
    socket->write(size + body);
    socket->flush();
}

// a whole message from the front of buffer, taken off it
static bool
takeMessage(QByteArray &buffer, QJsonObject &message)
{
    if (buffer.size() < 4) return false;
    quint32 size = qFromBigEndian<quint32>(buffer.constData());
    if (quint32(buffer.size() - 4) < size) return false;
    message = QJsonDocument::fromJson(buffer.mid(4, int(size))).object();
    buffer.remove(0, 4 + int(size));
    return true;
}

// the next message on a blocking socket, false when it closed first
static bool
readMessage(QLocalSocket &socket, QByteArray &buffer, QJsonObject &message)
{
    for (;;) {
        if (takeMessage(buffer, message)) return true;
        if (socket.bytesAvailable() == 0 && !socket.waitForReadyRead(-1) && socket.bytesAvailable() == 0)
            return false;
        buffer += socket.readAll();
    }
}

static QJsonObject
resultJson(const CommandResult &r)
{
    QJsonObject o;
    o.insert("status", int(r.status));
    if (!r.error.isEmpty()) o.insert("error", r.error);
    o.insert("data", r.data);
    if (!r.warnings.isEmpty()) o.insert("warnings", QJsonArray::fromStringList(r.warnings));
    if (!r.payload.isEmpty()) o.insert("payload", QString::fromLatin1(r.payload.toBase64()));
    if (!r.payloadType.isEmpty()) o.insert("payload_type", r.payloadType);
    if (!r.payloadName.isEmpty()) o.insert("payload_name", r.payloadName);
    if (!r.text.isEmpty()) o.insert("text", r.text);
    if (!r.csv.isEmpty()) o.insert("csv", r.csv);
    return o;
}

static CommandResult
resultFrom(const QJsonObject &o)
{
    CommandResult r;
    r.status = Status(o.value("status").toInt(int(Status::Internal)));
    r.error = o.value("error").toString();
    r.data = o.value("data").toObject();
    for (const QJsonValue &w : o.value("warnings").toArray()) r.warnings << w.toString();
    r.payload = QByteArray::fromBase64(o.value("payload").toString().toLatin1());
    r.payloadType = o.value("payload_type").toString();
    r.payloadName = o.value("payload_name").toString();
    r.text = o.value("text").toString();
    r.csv = o.value("csv").toString();
    return r;
}

static QJsonObject
readInfo(const QString &path)
{
    QFile file(path);
    if (!file.open(QFile::ReadOnly)) return QJsonObject();
    return QJsonDocument::fromJson(file.readAll()).object();
}

// a session's file whose process is still there
static bool
live(const QJsonObject &info)
{
    return !info.value("server").toString().isEmpty() && AthleteLock::processRunning(qint64(info.value("pid").toDouble()));
}

//
// The client side: the command line
//

QString
SessionClient::find(const QString &home, const QString &athlete)
{
    if (!athlete.isEmpty()) {
        QJsonObject info = readInfo(infoPath(athleteFolder(home, athlete)));
        return live(info) ? info.value("server").toString() : QString();
    }

    QString canonicalHome = QFileInfo(home).canonicalFilePath();
    if (canonicalHome.isEmpty()) return QString();
    QStringList servers;
    for (const QFileInfo &f : QDir(AthleteLock::runtimeDirectory()).entryInfoList({ "*.session" }, QDir::Files)) {
        QJsonObject info = readInfo(f.absoluteFilePath());
        if (live(info) && info.value("home").toString() == canonicalHome) servers << info.value("server").toString();
    }
    return servers.count() == 1 ? servers.first() : QString();
}

// one message to the session and the result it sends back. false when the
// session can't be reached, or is stopping: the message wasn't acted on
static bool
exchange(const QString &server, const QJsonObject &message,
         const std::function<void(const QString &)> &progress, CommandResult &result)
{
    QLocalSocket socket;
    socket.connectToServer(server);
    if (!socket.waitForConnected(2000)) return false;
    writeMessage(&socket, message);

    QByteArray buffer;
    QJsonObject answer;
    while (readMessage(socket, buffer, answer)) {
        if (answer.contains("progress")) {
            if (progress) progress(answer.value("progress").toString());
        } else if (answer.contains("result")) {
            result = resultFrom(answer.value("result").toObject());
            return true;
        } else if (answer.value("unavailable").toBool()) {
            return false;
        }
    }
    result = CommandResult::failure(Status::Failed, "the session stopped before the command finished");
    return true;
}

bool
SessionClient::run(const QString &server, const CommandRequest &request,
                   const std::function<void(const QString &)> &progress, CommandResult &result)
{
    QJsonObject r;
    r.insert("command", request.command);
    r.insert("args", request.args);
    r.insert("home", request.home);
    r.insert("athlete", request.athlete);
    // relative paths are the caller's
    r.insert("cwd", QDir::currentPath());
    r.insert("progress", bool(progress));
    QJsonObject message;
    message.insert("request", r);
    return exchange(server, message, progress, result);
}

CommandResult
SessionClient::status(const QString &server)
{
    CommandResult result;
    if (!exchange(server, QJsonObject{ { "status", true } }, nullptr, result))
        return CommandResult::failure(Status::NotFound, "the session is not running");
    return result;
}

CommandResult
SessionClient::stop(const QString &server)
{
    CommandResult result;
    if (!exchange(server, QJsonObject{ { "stop", true } }, nullptr, result))
        return CommandResult::failure(Status::NotFound, "the session is not running");
    return result;
}

//
// The server side: the session's process
//

// session start waits on this to hear whether the athlete opened
static void
notifyStarter(const CommandResult &result)
{
    QString name = qEnvironmentVariable("GC_SESSION_NOTIFY");
    if (name.isEmpty()) return;
    qunsetenv("GC_SESSION_NOTIFY");
    QLocalSocket socket;
    socket.connectToServer(name);
    if (!socket.waitForConnected(5000)) return;
    writeMessage(&socket, QJsonObject{ { "result", resultJson(result) } });
    socket.waitForBytesWritten(5000);
    socket.disconnectFromServer();
}

static QString
minutes(double m)
{
    return m == 1 ? QString("1 minute") : QString("%1 minutes").arg(m);
}

int
SessionServer::run(const Options &options)
{
    // the athlete first: a session that can't open it is no session
    SessionKeeper keeper;
    CommandResult failure;
    AthleteSession *opened = keeper.open(options.home, options.athlete, options.session, failure);
    if (!opened) {
        notifyStarter(failure);
        say(QString("error: %1\n").arg(failure.error));
        return int(failure.status);
    }
    const QString folder = opened->folder();
    const QString home = QFileInfo(options.home).canonicalFilePath();
    const int refreshed = opened->refreshedOnOpen();

    // it holds the athlete's lock, so a socket already there was left by a
    // session that has gone
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    const QString name = localName(folder, ".socket");
    if (!server.listen(name)) {
        QLocalServer::removeServer(name);
        if (!server.listen(name)) {
            CommandResult r = CommandResult::failure(Status::Failed, QString("can't listen on %1: %2").arg(name).arg(server.errorString()));
            notifyStarter(r);
            say(QString("error: %1\n").arg(r.error));
            return int(r.status);
        }
    }

    // how the command line finds us
    const QDateTime since = QDateTime::currentDateTime();
    QJsonObject info;
    info.insert("pid", double(QCoreApplication::applicationPid()));
    info.insert("home", home);
    info.insert("athlete", options.athlete);
    info.insert("folder", folder);
    info.insert("server", name);
    info.insert("since", since.toString(Qt::ISODate));
    info.insert("idle_minutes", options.idleMinutes);
    {
        QSaveFile file(infoPath(folder));
        if (!file.open(QFile::WriteOnly) || file.write(QJsonDocument(info).toJson()) < 0 || !file.commit()) {
            CommandResult r = CommandResult::failure(Status::Failed, QString("can't write %1").arg(infoPath(folder)));
            notifyStarter(r);
            say(QString("error: %1\n").arg(r.error));
            return int(r.status);
        }
    }

    QString until = options.idleMinutes > 0 ? QString(", or %1 without a command").arg(minutes(options.idleMinutes)) : QString();
    QJsonObject data;
    data.insert("athlete", options.athlete);
    data.insert("home", home);
    data.insert("pid", double(QCoreApplication::applicationPid()));
    data.insert("idle_minutes", options.idleMinutes);
    data.insert("refreshed", refreshed);
    CommandResult started = CommandResult::success(data);
    started.text = QString("session open for %1 (pid %2) until session stop%3\n")
                   .arg(options.athlete).arg(QCoreApplication::applicationPid()).arg(until);
    notifyStarter(started);
    if (!options.quiet) say(started.text);

    // commands run one at a time: the athlete's code turns nested event
    // loops, during which more requests may arrive. They wait their turn.
    struct Pending {
        QPointer<QLocalSocket> socket;
        QJsonObject message;
    };
    QList<Pending> queue;
    QHash<QLocalSocket *, QByteArray> buffers;
    bool busy = false, stopping = false;
    int commands = 0;
    QElapsedTimer idle;
    idle.start();

    auto status = [&]() {
        QJsonObject d;
        d.insert("athlete", options.athlete);
        d.insert("home", home);
        d.insert("folder", folder);
        d.insert("pid", double(QCoreApplication::applicationPid()));
        d.insert("since", since.toString(Qt::ISODate));
        d.insert("commands", commands);
        d.insert("idle_minutes", options.idleMinutes);
        d.insert("idle_seconds", qRound(idle.elapsed() / 1000.0));
        CommandResult r = CommandResult::success(d);
        r.text = QString("session for %1 (pid %2) open since %3, %4 command%5, last %6 s ago\n")
                 .arg(options.athlete).arg(QCoreApplication::applicationPid())
                 .arg(since.toString("yyyy-MM-dd HH:mm:ss")).arg(commands).arg(commands == 1 ? "" : "s")
                 .arg(qRound(idle.elapsed() / 1000.0));
        return r;
    };

    // closed and saved, no longer to be found: what session stop waits for
    auto shutdown = [&]() {
        stopping = true;
        server.close();
        QFile::remove(infoPath(folder));
        keeper.closeAll();
    };

    auto handle = [&](Pending &p) {
        QJsonObject m = p.message;
        if (stopping) {
            if (p.socket) writeMessage(p.socket, QJsonObject{ { "unavailable", true } });
            return;
        }
        if (m.value("status").toBool()) {
            if (p.socket) writeMessage(p.socket, QJsonObject{ { "result", resultJson(status()) } });
            return;
        }
        if (m.value("stop").toBool()) {
            CommandResult r = status();
            shutdown();
            r.data.insert("stopped", true);
            r.text = QString("session for %1 closed after %2 command%3\n")
                     .arg(options.athlete).arg(commands).arg(commands == 1 ? "" : "s");
            if (p.socket) {
                writeMessage(p.socket, QJsonObject{ { "result", resultJson(r) } });
                p.socket->waitForBytesWritten(5000);
            }
            return;
        }

        QJsonObject r = m.value("request").toObject();
        CommandRequest request;
        request.command = r.value("command").toString();
        request.args = r.value("args").toObject();
        request.home = options.home;
        request.athlete = options.athlete;

        // ours to run: commands on this athlete, and on the athletes folder
        // it is in (they change what the athlete uses, as in the GUI)
        CommandResult result;
        const Command *command = commandRegistry().find(request.command);
        bool athleteCommand = command && command->spec.scope == Scope::Athlete;
        if (QFileInfo(r.value("home").toString()).canonicalFilePath() != home
            || (athleteCommand && athleteFolder(r.value("home").toString(), r.value("athlete").toString()) != folder)) {
            result = CommandResult::failure(Status::Usage, QString("this session is for athlete '%1' in %2").arg(options.athlete).arg(home));
        } else {
            CommandRunner::Options run;
            run.session = options.session;
            run.keeper = &keeper;
            QPointer<QLocalSocket> socket = p.socket;
            if (r.value("progress").toBool())
                run.progress = [socket](const QString &text) { if (socket) writeMessage(socket, QJsonObject{ { "progress", text } }); };

            QElapsedTimer took;
            took.start();
            QString here = QDir::currentPath();
            QString cwd = r.value("cwd").toString();
            if (!cwd.isEmpty()) QDir::setCurrent(cwd);
            result = CommandRunner::run(commandRegistry(), request, run);
            QDir::setCurrent(here);
            // settings shared by all athletes, for commands run elsewhere
            // (an athlete's are synced when its command ends)
            if (command && command->spec.modifies && !athleteCommand) appsettings->syncQSettings();
            commands++;
            // idle is time without a command: asking how it is doesn't count
            idle.start();
            if (!options.quiet)
                say(QString("%1 %2 -> %3 (%4 ms)\n").arg(QDateTime::currentDateTime().toString(Qt::ISODate))
                    .arg(request.command).arg(int(result.status)).arg(took.elapsed()));
        }
        if (p.socket) writeMessage(p.socket, QJsonObject{ { "result", resultJson(result) } });
    };

    std::function<void()> drain = [&]() {
        // a command's nested event loop got us here: it drains when it's done
        if (busy) return;
        busy = true;
        while (!queue.isEmpty()) {
            Pending p = queue.takeFirst();
            handle(p);
        }
        busy = false;
        if (stopping) QCoreApplication::quit();
    };

    QObject::connect(&server, &QLocalServer::newConnection, &server, [&]() {
        while (QLocalSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [&, socket]() {
                QByteArray &buffer = buffers[socket];
                buffer += socket->readAll();
                QJsonObject message;
                bool added = false;
                while (takeMessage(buffer, message)) {
                    queue << Pending{ socket, message };
                    added = true;
                }
                // not from here: a command may delete a socket that closes
                // while it runs, this one included
                if (added) QMetaObject::invokeMethod(&server, drain, Qt::QueuedConnection);
            });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, [&, socket]() {
                buffers.remove(socket);
                socket->deleteLater();
            });
        }
    });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    QTimer poll;
    // not while a command runs: it may be turning a nested event loop
    QObject::connect(&poll, &QTimer::timeout, &poll, [&]() {
        if (busy || stopping) return;
        bool idleOut = options.idleMinutes > 0 && idle.elapsed() >= qint64(options.idleMinutes * 60000);
        if (!stopRequested && !idleOut) return;
        if (!options.quiet) say(QString("session for %1 closing: %2\n").arg(options.athlete).arg(idleOut ? "idle" : "interrupted"));
        shutdown();
        QCoreApplication::quit();
    });
    poll.start(200);

    QCoreApplication::exec();

    if (!stopping) shutdown();

    // connections still open go now: the server would delete them after
    // what their handlers use has gone
    for (QLocalSocket *socket : server.findChildren<QLocalSocket *>()) {
        QObject::disconnect(socket, nullptr, nullptr, nullptr);
        delete socket;
    }
    if (!options.quiet) say(QString("session for %1 closed after %2 command%3\n")
                            .arg(options.athlete).arg(commands).arg(commands == 1 ? "" : "s"));
    return int(Status::Ok);
}

// the last lines the session wrote before it went
static QString
logTail(const QString &path)
{
    QFile file(path);
    if (!file.open(QFile::ReadOnly)) return QString();
    if (file.size() > 4000) file.seek(file.size() - 4000);
    QStringList lines = QString::fromLocal8Bit(file.readAll()).split('\n', Qt::SkipEmptyParts);
    while (lines.count() > 5) lines.removeFirst();
    return lines.join("\n");
}

CommandResult
SessionServer::start(const Options &options, const QString &program, const QStringList &arguments)
{
    const QString folder = athleteFolder(options.home, options.athlete);

    // one is enough
    QString existing = SessionClient::find(options.home, options.athlete);
    if (!existing.isEmpty()) {
        CommandResult r = SessionClient::status(existing);
        if (r.ok()) {
            r.data.insert("already_open", true);
            r.text = "already open: " + r.text;
            return r;
        }
    }

    QLocalServer notify;
    notify.setSocketOptions(QLocalServer::UserAccessOption);
    const QString name = localName(folder, ".start");
    QLocalServer::removeServer(name);
    if (!notify.listen(name))
        return CommandResult::failure(Status::Failed, QString("can't start a session: %1").arg(notify.errorString()));

    QProcess child;
    child.setProgram(program);
    child.setArguments(arguments);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("GC_SESSION_NOTIFY", name);
    child.setProcessEnvironment(env);
    child.setStandardInputFile(QProcess::nullDevice());
    child.setStandardOutputFile(QProcess::nullDevice());
    child.setStandardErrorFile(logPath(folder));
    qint64 pid = 0;
    if (!child.startDetached(&pid))
        return CommandResult::failure(Status::Failed, QString("can't start a session: %1").arg(child.errorString()));

    // opening the athlete may take minutes on a slow folder: as long as the
    // session is alive it is getting there
    for (;;) {
        if (notify.waitForNewConnection(200)) {
            std::unique_ptr<QLocalSocket> socket(notify.nextPendingConnection());
            QByteArray buffer;
            QJsonObject message;
            if (socket && readMessage(*socket, buffer, message) && message.contains("result"))
                return resultFrom(message.value("result").toObject());
        }
        if (!AthleteLock::processRunning(pid) && !notify.hasPendingConnections()) {
            QString why = logTail(logPath(folder));
            return CommandResult::failure(Status::Failed, QString("the session stopped while opening the athlete%1")
                                                          .arg(why.isEmpty() ? QString() : ": " + why));
        }
    }
}

} // namespace Headless
