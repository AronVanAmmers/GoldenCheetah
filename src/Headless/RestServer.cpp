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

#include "RestServer.h"
#include "RestRouter.h"
#include "HeadlessCommands.h"
#include "ResultFormat.h"

#include "httplistener.h"
#include "httprequesthandler.h"
#include "httprequest.h"
#include "httpresponse.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <memory>
#include <csignal>
#include <cstdio>

namespace Headless {

static volatile std::sig_atomic_t stopRequested = 0;
static void onSignal(int) { stopRequested = 1; }

// runs commands on the thread that owns it (the main thread)
class RestDispatcher : public QObject
{
    public:
        explicit RestDispatcher(const RestServer::Options &options) : options(options) {}

        CommandResult execute(const CommandRequest &request) {
            CommandRunner::Options run;
            run.session = options.session;
            return CommandRunner::run(commandRegistry(), request, run);
        }

        RestServer::Options options;
};

class RestHandler : public HttpRequestHandler
{
    public:

        RestHandler(RestDispatcher *dispatcher, const RestServer::Options &options)
            : dispatcher(dispatcher), options(options), router(commandRegistry()) {}

        void service(HttpRequest &request, HttpResponse &response) override;

    private:

        void sendJson(HttpResponse &response, int status, const QJsonObject &body);
        void sendError(HttpResponse &response, int status, const QString &message);
        void log(const QString &method, const QString &path, int status, qint64 ms);

        RestDispatcher *dispatcher;
        RestServer::Options options;
        RestRouter router;

        // one command at a time: athlete sessions run nested event loops on
        // the main thread and must never interleave
        QMutex serial;
};

static QByteArray
reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 207: return "Multi-Status";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    default: return "Internal Server Error";
    }
}

void
RestHandler::log(const QString &method, const QString &path, int status, qint64 ms)
{
    if (options.quiet) return;
    fprintf(stderr, "%s %s %s -> %d (%lld ms)\n",
            QDateTime::currentDateTime().toString(Qt::ISODate).toLocal8Bit().constData(),
            method.toLocal8Bit().constData(), path.toLocal8Bit().constData(), status, (long long)ms);
    fflush(stderr);
}

void
RestHandler::sendJson(HttpResponse &response, int status, const QJsonObject &body)
{
    response.setStatus(status, reason(status));
    response.setHeader("Content-Type", "application/json; charset=utf-8");
    response.write(QJsonDocument(body).toJson(QJsonDocument::Compact), true);
}

void
RestHandler::sendError(HttpResponse &response, int status, const QString &message)
{
    QJsonObject o;
    o.insert("ok", false);
    o.insert("status", status == 404 ? "not_found" : status == 401 ? "unauthorized" : status == 403 ? "forbidden" : "usage");
    o.insert("error", message);
    sendJson(response, status, o);
}

void
RestHandler::service(HttpRequest &request, HttpResponse &response)
{
    QElapsedTimer timer;
    timer.start();
    QString method = QString::fromLatin1(request.getMethod()).toUpper();
    QString path = QString::fromUtf8(request.getPath());
    while (path.length() > 1 && path.endsWith("/")) path.chop(1);

    // only for this machine's own clients, not for web pages it happens to
    // be browsing: a page on another site may not call the API (its
    // requests carry an Origin), and the Host must be us (DNS rebinding)
    QString origin = QString::fromUtf8(request.getHeader("Origin"));
    QString host = QString::fromUtf8(request.getHeader("Host")).toLower();
    QStringList selves;
    for (const QString &name : { QString("127.0.0.1"), QString("localhost"), QString("[::1]"), options.host.toLower() })
        selves << QString("%1:%2").arg(name).arg(options.port);
    bool anyHost = options.host == "0.0.0.0" || options.host == "::";
    if (!origin.isEmpty() && !selves.contains(QUrl(origin).authority().toLower())) {
        sendError(response, 403, "requests from web pages on other sites are not allowed");
        log(method, path, 403, timer.elapsed());
        return;
    }
    if (!anyHost && !host.isEmpty() && !selves.contains(host)) {
        sendError(response, 403, QString("unexpected Host '%1'").arg(host));
        log(method, path, 403, timer.elapsed());
        return;
    }

    // authentication
    if (!options.token.isEmpty()) {
        QByteArray auth = request.getHeader("Authorization");
        if (auth != ("Bearer " + options.token).toUtf8()) {
            sendError(response, 401, "missing or wrong bearer token");
            log(method, path, 401, timer.elapsed());
            return;
        }
    }

    // the API describes itself
    QString base = QString("http://%1:%2%3").arg(options.host).arg(options.port).arg(RestRouter::prefix);
    if (method == "GET" && (path == "/v1" || path == "/v1/openapi.json" || path == "/")) {
        if (path == "/") {
            QJsonObject o;
            o.insert("name", "GoldenCheetah API");
            o.insert("api", base);
            o.insert("openapi", base + "/openapi.json");
            sendJson(response, 200, o);
        } else {
            sendJson(response, 200, router.openApi(base));
        }
        log(method, path, 200, timer.elapsed());
        return;
    }

    // uploaded files become paths in a temporary folder that lives until
    // the command is done
    // created when the first file arrives
    QJsonArray uploadedPaths;
    std::unique_ptr<QTemporaryDir> uploadDir;
    auto uploadFolder = [&]() -> QString {
        if (!uploadDir) uploadDir.reset(new QTemporaryDir());
        QString folder = uploadDir->filePath(QString::number(uploadedPaths.count() + 1));
        QDir().mkpath(folder);
        return folder;
    };
    QMultiMap<QString, QString> query;
    QMultiMap<QByteArray, QByteArray> params = request.getParameterMap();
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
        QTemporaryFile *file = request.getUploadedFile(it.key());
        if (file) {
            QString name = QFileInfo(QString::fromUtf8(it.value())).fileName();
            if (name.isEmpty()) name = QString("upload-%1").arg(uploadedPaths.count() + 1);
            // keep the original name: the import takes the start time from
            // file names like 2024_01_31_10_00_00.fit, as the GUI does
            QString target = QDir(uploadFolder()).filePath(name);
            file->seek(0);
            QFile out(target);
            if (out.open(QFile::WriteOnly)) {
                while (!file->atEnd()) out.write(file->read(1 << 20));
                out.close();
                uploadedPaths.append(target);
            }
            continue;
        }
        query.insert(QString::fromUtf8(it.key()), QString::fromUtf8(it.value()));
    }

    QByteArray body = request.getBody();
    QByteArray contentType = request.getHeader("Content-Type").toLower();
    bool formLike = contentType.isEmpty() || contentType.startsWith("application/x-www-form-urlencoded");
    bool looksJson = contentType.startsWith("application/json");
    bool rawUpload = !looksJson && !body.isEmpty() && !contentType.contains("multipart")
                     && (!formLike || query.contains("filename") || !request.getHeader("X-Filename").isEmpty());

    // curl -d '{...}' sends form data: say what to do rather than guessing
    if (formLike && !rawUpload && body.trimmed().startsWith("{")) {
        sendError(response, 400, "send JSON with 'Content-Type: application/json'");
        log(method, path, 400, timer.elapsed());
        return;
    }

    // curl -d and --data-binary send x-www-form-urlencoded by default, and the
    // http library then reads the body as form parameters too: undo that when
    // the body is really JSON or a file
    if (formLike && !body.isEmpty() && (looksJson || rawUpload)) {
        for (const QByteArray &part : body.split('&')) {
            int eq = part.indexOf('=');
            QString name = QString::fromUtf8(HttpRequest::urlDecode(eq >= 0 ? part.left(eq).trimmed() : part));
            QString value = QString::fromUtf8(HttpRequest::urlDecode(eq >= 0 ? part.mid(eq + 1).trimmed() : QByteArray()));
            query.remove(name, value);
        }
    }

    QByteArray jsonBody;
    if (looksJson) {
        jsonBody = body;
    } else if (rawUpload) {
        // a raw file upload, e.g. curl --data-binary @ride.fit '...?filename=ride.fit'
        QString name = QFileInfo(query.value("filename")).fileName();
        if (name.isEmpty()) name = QFileInfo(QString::fromUtf8(request.getHeader("X-Filename"))).fileName();
        if (name.isEmpty()) {
            sendError(response, 400, "give the uploaded file's name with ?filename= or an X-Filename header");
            log(method, path, 400, timer.elapsed());
            return;
        }
        query.remove("filename");
        QString target = QDir(uploadFolder()).filePath(name);
        QFile out(target);
        if (out.open(QFile::WriteOnly)) {
            out.write(body);
            out.close();
            uploadedPaths.append(target);
        }
    }

    RestRouter::Match match = router.match(method, path, query, jsonBody);
    if (match.httpStatus != 200) {
        if (match.httpStatus == 405) response.setHeader("Allow", match.allowed.join(", ").toLatin1());
        sendError(response, match.httpStatus, match.error);
        log(method, path, match.httpStatus, timer.elapsed());
        return;
    }

    // uploads feed the command's file parameter
    if (!uploadedPaths.isEmpty()) {
        const Command *c = commandRegistry().find(match.command);
        if (!c || !c->spec.param("file")) {
            sendError(response, 400, QString("'%1' does not take uploaded files").arg(match.command));
            log(method, path, 400, timer.elapsed());
            return;
        }
        QJsonArray files = match.args.value("file").isArray() ? match.args.value("file").toArray() : QJsonArray();
        if (match.args.value("file").isString()) files.append(match.args.value("file"));
        for (const QJsonValue &v : uploadedPaths) files.append(v);
        match.args.insert("file", files);
    }

    // binary results (charts, exports) come back as files unless the
    // caller asks for the JSON envelope
    bool wantEnvelope = match.args.value("envelope").toVariant().toBool();
    match.args.remove("envelope");

    CommandRequest cmd;
    cmd.command = match.command;
    cmd.args = match.args;
    cmd.home = options.home;
    cmd.athlete = match.athlete;

    CommandResult result;
    {
        QMutexLocker locker(&serial);
        QMetaObject::invokeMethod(dispatcher, [&]() { result = dispatcher->execute(cmd); }, Qt::BlockingQueuedConnection);
    }

    // report uploads by the name the client sent, not our temporary path
    if (uploadDir && result.data.value("files").isArray()) {
        QString prefix = QDir(uploadDir->path()).absolutePath() + "/";
        QJsonArray files = result.data.value("files").toArray();
        for (int i = 0; i < files.count(); i++) {
            QJsonObject f = files.at(i).toObject();
            QString source = f.value("source").toString();
            if (source.startsWith(prefix)) f.insert("source", source.mid(prefix.length()).section('/', 1));
            files[i] = f;
        }
        result.data.insert("files", files);
    }

    int status = httpStatusFor(result.status);
    if (!result.payload.isEmpty() && !wantEnvelope && result.ok()) {
        response.setStatus(200, "OK");
        response.setHeader("Content-Type", result.payloadType.toLatin1());
        response.setHeader("Content-Disposition", QString("inline; filename=\"%1\"").arg(result.payloadName).toUtf8());
        response.write(result.payload, true);
    } else {
        if (!result.payload.isEmpty()) result.data.insert("payload_bytes", result.payload.size());
        sendJson(response, status, ResultFormat::envelope(cmd.command, result));
    }
    log(method, path, status, timer.elapsed());
}

int
RestServer::run(const Options &options)
{
    // anyone on the network could run commands, including Python
    bool loopback = options.host == "127.0.0.1" || options.host == "localhost" || options.host == "::1";
    if (!loopback && options.token.isEmpty()) {
        fprintf(stderr, "error: listening on %s needs a token (--token or GC_API_TOKEN)\n", options.host.toLocal8Bit().constData());
        return int(Status::Usage);
    }

    // the listener reads its configuration from QSettings
    QTemporaryDir config;
    QSettings *settings = new QSettings(config.filePath("httpserver.ini"), QSettings::IniFormat);
    settings->setValue("host", options.host);
    settings->setValue("port", options.port);
    settings->setValue("minThreads", 1);
    settings->setValue("maxThreads", 8);
    settings->setValue("cleanupInterval", 60000);
    settings->setValue("readTimeout", 120000);
    settings->setValue("maxRequestSize", qint64(options.maxUploadBytes));
    settings->setValue("maxMultiPartSize", qint64(options.maxUploadBytes));
    settings->sync();

    RestDispatcher dispatcher(options);
    RestHandler *handler = new RestHandler(&dispatcher, options);
    HttpListener *listener = new HttpListener(settings, handler, QCoreApplication::instance());

    if (!listener->isListening()) {
        fprintf(stderr, "error: can't listen on %s:%d\n", options.host.toLocal8Bit().constData(), options.port);
        return int(Status::Failed);
    }

    if (!options.quiet) {
        fprintf(stderr, "GoldenCheetah API on http://%s:%d/v1 for %s%s (ctrl-c to stop)\n",
                options.host.toLocal8Bit().constData(), options.port,
                options.home.toLocal8Bit().constData(),
                options.token.isEmpty() ? "" : ", bearer token required");
        fflush(stderr);
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, []() { if (stopRequested) QCoreApplication::quit(); });
    poll.start(200);

    int ret = QCoreApplication::exec();

    listener->close();
    delete listener;
    delete handler;
    delete settings;
    if (!options.quiet) fprintf(stderr, "GoldenCheetah API stopped\n");
    return ret == 0 ? int(Status::Ok) : int(Status::Failed);
}

} // namespace Headless
