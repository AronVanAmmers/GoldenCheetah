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
// Downloading activities from a cloud account, headless.
//
// The same path as Share → Synchronise Activities and the startup
// auto-download: open the account, list files, read the ones whose start
// time is not already stored, and save them as the sync dialog saves them.
//

#include "HeadlessCommands.h"
#include "SeasonRange.h"

#include "Context.h"
#include "Athlete.h"
#include "RideCache.h"
#include "RideItem.h"
#include "RideFile.h"
#include "RideMetadata.h"
#include "JsonRideFile.h"
#include "DataProcessor.h"
#include "CloudService.h"
#include "Settings.h"

#include <QDate>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSet>
#include <QTimer>

namespace Headless {

static const int kReadTimeoutMs = 120000;

static QString
activityStem(const QDateTime &t)
{
    const QChar zero('0');
    return QString("%1_%2_%3_%4_%5_%6")
           .arg(t.date().year(), 4, 10, zero)
           .arg(t.date().month(), 2, 10, zero)
           .arg(t.date().day(), 2, 10, zero)
           .arg(t.time().hour(), 2, 10, zero)
           .arg(t.time().minute(), 2, 10, zero)
           .arg(t.time().second(), 2, 10, zero);
}

static const CloudService *
findService(const QString &want, QString &error)
{
    CloudServiceFactory &factory = CloudServiceFactory::instance();
    const CloudService *found = nullptr;
    QStringList names;
    for (const QString &name : factory.serviceNames()) {
        const CloudService *s = factory.service(name);
        if (!s) continue;
        names << s->id();
        if (s->id().compare(want, Qt::CaseInsensitive) != 0 && s->uiName().compare(want, Qt::CaseInsensitive) != 0)
            continue;
        if (found && found->id() != s->id()) {
            error = QString("'%1' matches more than one cloud account").arg(want);
            return nullptr;
        }
        found = s;
    }
    if (!found) error = QString("no cloud account '%1'. Accounts: %2").arg(want, names.join(", "));
    return found;
}

static bool
canListActivities(const CloudService *s)
{
    return s && (s->type() & CloudService::Activities) && (s->capabilities() & CloudService::Query);
}

// write the ride the way the sync dialog and the startup download do
static bool
saveDownloadedRide(Context *context, RideFile *ride, QString &stem, QString &message)
{
    if (!ride->startTime().isValid()) {
        message = "the activity has no start time";
        return false;
    }
    stem = activityStem(ride->startTime());
    const QString filename = context->athlete->home->activities().canonicalPath() + "/" + stem + ".json";
    if (QFileInfo(filename).exists()) {
        message = "already stored";
        return false;
    }

    GlobalContext::context()->rideMetadata->setLinkedDefaults(ride);
    DataProcessorFactory::instance().autoProcess(ride, "Auto", "Import");
    ride->recalculateDerivedSeries();
    DataProcessorFactory::instance().autoProcess(ride, "Save", "ADD");

    JsonFileReader reader;
    QFile file(filename);
    if (!reader.writeRideFile(context, ride, file)) {
        message = "could not write the activity";
        return false;
    }
    context->athlete->addRide(QFileInfo(filename).fileName(), true, false);
    return true;
}

struct FileResult {
    QString file;
    QString status;     // downloaded, skipped, failed
    QString activity;
    QString start;
    QString message;

    QJsonObject json() const {
        QJsonObject o;
        o.insert("file", file);
        o.insert("status", status);
        if (!activity.isEmpty()) o.insert("activity", activity);
        if (!start.isEmpty()) o.insert("start", start);
        if (!message.isEmpty()) o.insert("message", message);
        return o;
    }
};

static FileResult
downloadOne(Context *context, CloudService *service, CloudServiceEntry *entry, QSet<QString> &have)
{
    FileResult result;
    result.file = entry->name;

    QDateTime named;
    if (RideFile::parseRideFileName(QFileInfo(entry->name).fileName(), &named)) {
        const QString stem = activityStem(named);
        if (have.contains(stem)) {
            result.status = "skipped";
            result.activity = stem;
            result.start = named.toString("yyyy-MM-ddTHH:mm:ss");
            result.message = "already stored";
            return result;
        }
    }

    QByteArray *data = new QByteArray;
    bool done = false;
    QString gotName = entry->name;
    QString gotMessage;
    QByteArray *got = nullptr;

    QEventLoop loop;
    QObject waiter;
    QObject::connect(service, &CloudService::readComplete, &waiter,
                     [&](QByteArray *d, const QString &name, const QString &message) {
        got = d;
        gotName = name;
        gotMessage = message;
        done = true;
        loop.quit();
    });

    const bool started = service->readFile(data, entry->name, entry->id);
    if (!done && started) {
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(kReadTimeoutMs);
        loop.exec();
    }

    if (!done) {
        result.status = "failed";
        result.message = started ? "timed out waiting for the file" : (gotMessage.isEmpty() ? "the account did not return the file" : gotMessage);
        // a download still in flight writes this buffer; only free it when nothing will
        if (!started) delete data;
        return result;
    }

    QByteArray *body = got ? got : data;
    QStringList errors;
    RideFile *ride = service->uncompressRide(body, gotName, errors);
    if (body != data) delete data;
    delete body;

    if (!ride) {
        result.status = "failed";
        result.message = errors.isEmpty() ? (gotMessage.isEmpty() ? "could not read the activity" : gotMessage) : errors.join("; ");
        return result;
    }

    QString stem, message;
    const bool saved = saveDownloadedRide(context, ride, stem, message);
    const QString start = ride->startTime().isValid() ? ride->startTime().toString("yyyy-MM-ddTHH:mm:ss") : QString();
    delete ride;

    result.activity = stem;
    result.start = start;
    if (saved) {
        result.status = "downloaded";
        have.insert(stem);
    } else if (message == "already stored") {
        result.status = "skipped";
        result.message = message;
        if (!stem.isEmpty()) have.insert(stem);
    } else {
        result.status = "failed";
        result.message = message;
    }
    return result;
}

static CommandResult
downloadActivities(CommandEnvironment &env, const CommandRequest &request)
{
    AthleteSession &session = *env.session;
    Context *context = session.context();

    QStringList wanted;
    if (request.args.contains("service")) {
        for (const QJsonValue &v : request.args.value("service").toArray()) {
            QString error;
            const CloudService *s = findService(v.toString(), error);
            if (!s) return CommandResult::failure(Status::NotFound, error);
            if (!canListActivities(s))
                return CommandResult::failure(Status::Usage, QString("%1 cannot list activities to download").arg(s->id()));
            if (!wanted.contains(s->id())) wanted << s->id();
        }
    } else {
        CloudServiceFactory &factory = CloudServiceFactory::instance();
        for (const QString &name : factory.serviceNames()) {
            const CloudService *s = factory.service(name);
            if (!canListActivities(s)) continue;
            const QString active = appsettings->cvalue(context->athlete->cyclist, s->activeSettingName(), "false").toString();
            if (active == "true") wanted << s->id();
        }
        if (wanted.isEmpty())
            return CommandResult::failure(Status::Failed,
                        "no cloud activity account is turned on. Pass --service NAME for an account that can list activities.");
    }

    QDate fromDay, toDay;
    QString error;
    Status status = Status::Ok;
    if (!dateRangeArgs(session, request.args, fromDay, toDay, error, status)) return CommandResult::failure(status, error);
    const bool datesGiven = request.args.contains("from") || request.args.contains("to") || request.args.contains("season");
    if (!datesGiven) {
        toDay = QDate::currentDate();
        fromDay = toDay.addDays(-30);
    } else {
        if (!toDay.isValid()) toDay = QDate::currentDate();
        if (!fromDay.isValid()) fromDay = toDay.addDays(-30);
    }
    if (fromDay > toDay) return CommandResult::failure(Status::Usage, "--from is after --to");

    QSet<QString> have;
    for (RideItem *item : session.rideCache()->rides()) {
        if (!item || item->planned) continue;
        have.insert(QFileInfo(item->fileName).completeBaseName());
    }

    const QDateTime rangeFrom(fromDay, QTime(0, 0));
    const QDateTime rangeTo(toDay, QTime(23, 59, 59));

    QJsonArray services;
    QString text;
    QStringList problems;
    int downloaded = 0, skipped = 0, failed = 0, ignored = 0;

    for (const QString &id : wanted) {
        CloudService *service = CloudServiceFactory::instance().newService(id, context);
        bool opened = false;
        QStringList openErrors;
        if (service->open(openErrors)) opened = true;

        auto finish = [&]() {
            if (opened) service->close();
            delete service;
        };

        QJsonObject one;
        one.insert("service", id);
        if (!opened) {
            const QString message = openErrors.isEmpty() ? QString("could not connect") : openErrors.join("; ");
            one.insert("status", "failed");
            one.insert("message", message);
            services.append(one);
            failed++;
            problems << QString("%1: %2").arg(id, message);
            text += QString("%1: %2\n").arg(id, message);
            finish();
            continue;
        }

        env.report(QString("%1: listing activities").arg(service->uiName()));
        QStringList listErrors;
        const QList<CloudServiceEntry *> found = service->readdir(service->home(), listErrors, rangeFrom, rangeTo);

        QJsonArray files;
        int got = 0, already = 0, bad = 0, outside = 0;
        for (CloudServiceEntry *entry : found) {
            if (!entry || entry->name.isEmpty()) continue;
            QDateTime named;
            if (!RideFile::parseRideFileName(QFileInfo(entry->name).fileName(), &named)) continue;
            if (named.date() < fromDay || named.date() > toDay) { outside++; ignored++; continue; }

            env.report(QString("%1: %2").arg(service->uiName(), entry->name));
            FileResult item = downloadOne(context, service, entry, have);
            files.append(item.json());
            if (item.status == "downloaded") { got++; downloaded++; }
            else if (item.status == "skipped") { already++; skipped++; }
            else { bad++; failed++; problems << QString("%1: %2 (%3)").arg(id, entry->name, item.message); }
        }

        one.insert("downloaded", got);
        one.insert("skipped", already);
        one.insert("failed", bad);
        one.insert("outside", outside);
        one.insert("files", files);
        if (!listErrors.isEmpty()) one.insert("warnings", QJsonArray::fromStringList(listErrors));
        services.append(one);

        text += QString("%1: %2 downloaded, %3 already stored").arg(id).arg(got).arg(already);
        if (bad) text += QString(", %1 failed").arg(bad);
        if (outside) text += QString(", %1 outside the dates").arg(outside);
        text += "\n";
        for (const QJsonValue &v : files) {
            const QJsonObject row = v.toObject();
            if (row.value("status").toString() == "skipped") continue;
            text += QString("  %1  %2").arg(row.value("status").toString(), row.value("file").toString());
            if (row.contains("activity")) text += "  " + row.value("activity").toString();
            if (!row.value("message").toString().isEmpty()) text += "  " + row.value("message").toString();
            text += "\n";
        }
        finish();
    }

    // a new activity's metrics were computed as it was saved; wait out anything still running
    if (downloaded) session.waitForRefresh();

    QJsonObject data;
    data.insert("from", fromDay.toString(Qt::ISODate));
    data.insert("to", toDay.toString(Qt::ISODate));
    data.insert("downloaded", downloaded);
    data.insert("skipped", skipped);
    data.insert("failed", failed);
    data.insert("ignored", ignored);
    data.insert("services", services);

    const int attempts = downloaded + skipped + failed;
    CommandResult result = attempts == 0
        ? CommandResult::success(data)
        : CommandResult::batch(data, failed, attempts, problems.join("; "));
    result.text = text + QString("%1 downloaded, %2 already stored, %3 failed\n").arg(downloaded).arg(skipped).arg(failed);
    return result;
}

void
registerCloudCommands(CommandRegistry &registry)
{
    Command download;
    download.spec.name = "activity.download";
    download.spec.summary = "download activities from a cloud account";
    download.spec.description =
        "The same download as Share → Synchronise Activities. The account is opened\n"
        "with the authorization stored in Preferences, and activities in the date\n"
        "range that are not already stored are saved. An activity already stored\n"
        "(the same start time) is left as it is. With no --service, every turned-on\n"
        "account that can list activities is used. With no dates, the last 30 days\n"
        "through today are fetched. --from and --to are whole days.";
    download.spec.scope = Scope::Athlete;
    download.spec.modifies = true;
    download.spec.params << ParamSpec("service", ParamType::String, "cloud account, e.g. Strava or \"Local Store\" (repeat for several)").many();
    download.spec.params << ParamSpec("from", ParamType::Date, "first day, inclusive");
    download.spec.params << ParamSpec("to", ParamType::Date, "last day, inclusive");
    download.spec.params << seasonParam();
    download.spec.httpMethod = "POST";
    download.spec.httpPath = "/athletes/{athlete}/activities/download";
    download.handler = downloadActivities;
    registry.add(download);
}

} // namespace Headless
