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

#include "HeadlessCommands.h"
#include "ActivitySelection.h"
#include "MetricData.h"
#include "ZoneData.h"

#include "Context.h"
#include "Athlete.h"
#include "RideCache.h"
#include "RideItem.h"
#include "RideFile.h"
#include "IntervalItem.h"
#include "RideMetric.h"
#include "RideMetadata.h"
#include "DataFilter.h"
#include "CsvRideFile.h"
#include "PMCData.h"

#include <QFileInfo>
#include <QTemporaryDir>
#include <cmath>

namespace Headless {

static CommandResult
listActivities(CommandEnvironment &env, const CommandRequest &request)
{
    ActivitySelection selection = ActivitySelection::fromArgs(request.args);
    QList<RideItem *> items;
    QString error;
    Status status;
    if (!selection.resolve(*env.session, items, error, status)) return CommandResult::failure(status, error);

    QStringList metrics = splitList(request.args.value("metric"));
    QStringList fields = splitList(request.args.value("field"));
    bool metricUnits = !request.args.value("imperial").toBool(false);

    for (const QString &m : metrics)
        if (!RideMetricFactory::instance().haveMetric(m))
            return CommandResult::failure(Status::Usage, QString("unknown metric '%1', see 'metric list'").arg(m));

    QJsonArray list;
    for (RideItem *item : items) {
        QJsonObject o = activitySummary(item);
        addMetrics(o, item, metrics, metricUnits);
        if (!fields.isEmpty()) addMetadata(o, item, fields);
        list.append(o);
    }
    QJsonObject data;
    data.insert("count", list.count());
    data.insert("activities", list);
    return CommandResult::success(data);
}

static CommandResult
showActivity(CommandEnvironment &env, const CommandRequest &request)
{
    QString error;
    RideItem *item = env.session->findActivity(request.args.value("activity").toString(), error);
    if (!item) return CommandResult::failure(Status::NotFound, error);
    bool metricUnits = !request.args.value("imperial").toBool(false);

    QJsonObject o = activitySummary(item);
    addMetadata(o, item, QStringList());

    // every metric that has a value
    const RideMetricFactory &factory = RideMetricFactory::instance();
    QJsonObject metrics;
    for (int i = 0; i < factory.metricCount(); i++) {
        QString symbol = factory.metricName(i);
        double v = item->getForSymbol(symbol, metricUnits);
        if (std::isnan(v) || std::isinf(v) || v == 0) continue;
        metrics.insert(symbol, jsonNumber(v));
    }
    o.insert("metrics", metrics);

    // time in zones, as the overview's zone tables
    QJsonObject zones;
    for (const QString &type : zoneTypes()) {
        ActivityZones z;
        QString ignored;
        if (!activityZones(env.session->athlete(), item, type, z, ignored)) continue;
        // only zones for data the activity has
        bool any = false;
        for (const ZoneRow &r : z.rows) if (r.seconds > 0) any = true;
        if (any) zones.insert(type, activityZonesJson(env.session->athlete(), z, metricUnits));
    }
    o.insert("zones", zones);

    // form, fitness, fatigue and risk on the day, as the overview's PMC tile
    // the default overview tiles use GOVSS for runs and SwimScore for swims
    QString pmcMetric = request.args.value("pmc-metric").toString();
    if (pmcMetric.isEmpty()) pmcMetric = item->isRun ? "govss" : item->isSwim ? "swimscore" : "coggan_tss";
    if (!RideMetricFactory::instance().haveMetric(pmcMetric))
        return CommandResult::failure(Status::Usage, QString("unknown metric '%1', see 'metric list'").arg(pmcMetric));
    PMCData *pmc = pmcFor(*env.session, pmcMetric, -1, -1);
    if (pmc) {
        QDate day = item->dateTime.date();
        QJsonObject p;
        p.insert("metric", pmcMetric);
        p.insert("stress", jsonNumber(pmc->stress(day)));
        p.insert("ctl", jsonNumber(pmc->lts(day)));
        p.insert("atl", jsonNumber(pmc->sts(day)));
        p.insert("tsb", jsonNumber(pmc->sb(day)));
        p.insert("rr", jsonNumber(pmc->rr(day)));
        o.insert("pmc", p);
    }

    // in brief, 'interval list' and 'interval show' have their metrics
    QJsonArray intervals;
    int number = 0;
    for (IntervalItem *i : item->intervals()) {
        QJsonObject io;
        io.insert("number", ++number);
        io.insert("name", i->name);
        io.insert("type", RideFileInterval::typeDescription(i->type));
        io.insert("start", i->start);
        io.insert("stop", i->stop);
        intervals.append(io);
    }
    o.insert("intervals", intervals);

    RideFile *ride = item->ride();
    if (ride) {
        o.insert("samples", ride->dataPoints().count());
        o.insert("recording_interval", ride->recIntSecs());
        o.insert("device", ride->deviceType());
        QJsonObject xdata;
        for (auto it = ride->xdata().constBegin(); it != ride->xdata().constEnd(); ++it)
            xdata.insert(it.key(), QJsonArray::fromStringList(it.value()->valuename));
        if (!xdata.isEmpty()) o.insert("xdata", xdata);
    }
    return CommandResult::success(o);
}

static CommandResult
exportActivity(CommandEnvironment &env, const CommandRequest &request)
{
    QString error;
    RideItem *item = env.session->findActivity(request.args.value("activity").toString(), error);
    if (!item) return CommandResult::failure(Status::NotFound, error);

    QString format = request.args.value("as").toString().toLower();
    RideFile *ride = item->ride();
    if (!ride) return CommandResult::failure(Status::Failed, "can't open the activity file");

    const RideFileFactory &factory = RideFileFactory::instance();
    bool special = format == "csv-gc" || format == "csv-wprime";
    if (!special && !factory.writeSuffixes().contains(format))
        return CommandResult::failure(Status::Usage,
                    QString("can't export as '%1', choose one of: %2, csv-gc, csv-wprime")
                    .arg(format).arg(factory.writeSuffixes().join(", ")));

    QTemporaryDir tmp;
    QString suffix = special ? QString("csv") : format;
    QString path = tmp.filePath("export." + suffix);
    QFile file(path);
    bool ok;
    if (format == "csv-gc") {
        CsvFileReader writer;
        ok = writer.writeRideFile(env.session->context(), ride, file, CsvFileReader::gc);
    } else if (format == "csv-wprime") {
        CsvFileReader writer;
        ok = writer.writeRideFile(env.session->context(), ride, file, CsvFileReader::wprime);
    } else {
        ok = factory.writeRideFile(env.session->context(), ride, file, format);
    }
    if (!ok || !file.open(QFile::ReadOnly)) return CommandResult::failure(Status::Failed, "export failed");

    CommandResult result;
    result.payload = file.readAll();
    result.payloadName = QFileInfo(item->fileName).completeBaseName() + "." + suffix;
    static const QMap<QString,QString> types = {
        { "json", "application/json" }, { "csv", "text/csv" }, { "tcx", "application/vnd.garmin.tcx+xml" },
        { "gpx", "application/gpx+xml" }, { "fit", "application/vnd.ant.fit" }, { "pwx", "application/xml" },
        { "gc", "application/xml" }, { "fitlog", "application/xml" }
    };
    result.payloadType = types.value(suffix, "application/octet-stream");
    result.data.insert("activity", QFileInfo(item->fileName).completeBaseName());
    result.data.insert("format", format);
    result.data.insert("bytes", result.payload.size());
    return result;
}

static CommandResult
setFields(CommandEnvironment &env, const CommandRequest &request)
{
    ActivitySelection selection = ActivitySelection::fromArgs(request.args);
    if (selection.isEmpty() && !request.args.value("all").toBool(false))
        return CommandResult::failure(Status::Usage, "choose activities (by name, --filter, --from ...) or pass --all");

    // NAME=VALUE pairs, an empty value removes the field
    QList<QPair<QString,QString>> assignments;
    QMap<QString, FieldDefinition> defs;
    for (const FieldDefinition &f : GlobalContext::context()->rideMetadata->getFields()) defs.insert(f.name, f);

    for (const QJsonValue &v : request.args.value("set").toArray()) {
        QString text = v.toString();
        int eq = text.indexOf('=');
        if (eq <= 0) return CommandResult::failure(Status::Usage, QString("expected NAME=VALUE, got '%1'").arg(text));
        QString name = text.left(eq).trimmed(), value = text.mid(eq + 1);

        if (defs.contains(name) && defs.value(name).isNumericField() && !value.isEmpty()) {
            GcFieldType t = defs.value(name).type;
            if (t == GcFieldType::FIELD_INTEGER || t == GcFieldType::FIELD_DOUBLE || t == GcFieldType::FIELD_CHECKBOX) {
                bool ok = false;
                value.trimmed().toDouble(&ok);
                if (!ok) return CommandResult::failure(Status::Usage, QString("field '%1' is numeric, '%2' is not a number").arg(name).arg(value));
            }
        }
        if (!defs.contains(name) && !request.args.value("allow-undefined").toBool(false))
            return CommandResult::failure(Status::Usage,
                        QString("'%1' is not a defined field, add it with 'field add' or pass --allow-undefined").arg(name));
        assignments << qMakePair(name, value);
    }

    QList<RideItem *> items;
    QString error;
    Status status;
    if (!selection.resolve(*env.session, items, error, status)) return CommandResult::failure(status, error);

    QList<RideItem *> changed;
    QJsonArray report;
    for (RideItem *item : items) {
        RideFile *ride = item->ride();
        QJsonObject r;
        r.insert("activity", QFileInfo(item->fileName).completeBaseName());
        if (!ride) {
            r.insert("status", "failed");
            report.append(r);
            continue;
        }
        bool any = false;
        for (const auto &a : assignments) {
            QString before = ride->getTag(a.first, QString());
            if (a.second.isEmpty()) {
                if (ride->removeTag(a.first)) any = true;
            } else if (before != a.second) {
                ride->setTag(a.first, a.second);
                any = true;
            }
        }
        r.insert("status", any ? "updated" : "unchanged");
        report.append(r);
        if (any) {
            item->notifyRideMetadataChanged();
            item->setDirty(true);
            changed << item;
        }
    }

    if (!changed.isEmpty()) {
        QString saveError;
        if (!env.session->rideCache()->saveActivities(changed, saveError))
            return CommandResult::failure(Status::Failed, saveError);
        env.session->refresh();
    }

    QJsonObject data;
    data.insert("activities", report);
    data.insert("updated", changed.count());
    return CommandResult::success(data);
}

static CommandResult
deleteActivities(CommandEnvironment &env, const CommandRequest &request)
{
    QStringList ids;
    for (const QJsonValue &v : request.args.value("activity").toArray()) ids << v.toString();

    QStringList files;
    for (const QString &id : ids) {
        QString error;
        RideItem *item = env.session->findActivity(id, error);
        if (!item) return CommandResult::failure(Status::NotFound, error);
        files << item->fileName;
    }
    files.removeDuplicates();

    QJsonArray deleted;
    for (const QString &f : files) {
        if (env.session->rideCache()->removeRide(f)) deleted.append(QFileInfo(f).completeBaseName());
    }
    QJsonObject data;
    data.insert("deleted", deleted);
    data.insert("backup", "activities are moved to the athlete's bak folder");
    CommandResult result = CommandResult::success(data);
    if (deleted.count() != files.count()) {
        result.status = Status::Partial;
        result.error = "some activities could not be deleted";
    }
    return result;
}

static CommandResult
evalActivities(CommandEnvironment &env, const CommandRequest &request)
{
    QString expression = request.args.value("expression").toString();
    DataFilter df(nullptr, env.session->context(), expression);
    if (!df.getErrors().isEmpty())
        return CommandResult::failure(Status::Usage, QString("bad expression: %1").arg(df.getErrors().join("; ")));

    ActivitySelection selection = ActivitySelection::fromArgs(request.args);
    QList<RideItem *> items;
    QString error;
    Status status;
    if (!selection.resolve(*env.session, items, error, status)) return CommandResult::failure(status, error);

    QJsonArray list;
    for (RideItem *item : items) {
        Result r = df.evaluate(item, nullptr);
        QJsonObject o;
        o.insert("activity", QFileInfo(item->fileName).completeBaseName());
        o.insert("start", activityStart(item));
        if (r.isNumber) {
            double v = r.number();
            o.insert("value", std::isfinite(v) ? QJsonValue(v) : QJsonValue());
        } else {
            o.insert("value", r.string());
        }
        list.append(o);
    }
    QJsonObject data;
    data.insert("expression", expression);
    data.insert("activities", list);
    return CommandResult::success(data);
}

void
registerActivityCommands(CommandRegistry &registry)
{
    Command list;
    list.spec.name = "activity.list";
    list.spec.summary = "list activities, optionally with metrics and fields";
    list.spec.scope = Scope::Athlete;
    list.spec.params << ActivitySelection::params(true);
    list.spec.params << ParamSpec("metric", ParamType::String, "metric symbols to include (comma separated or repeated)").many();
    list.spec.params << ParamSpec("field", ParamType::String, "metadata fields to include").many();
    list.spec.params << ParamSpec("imperial", ParamType::Bool, "metric values in imperial units");
    list.spec.httpMethod = "GET";
    list.spec.httpPath = "/athletes/{athlete}/activities";
    list.handler = listActivities;
    registry.add(list);

    Command show;
    show.spec.name = "activity.show";
    show.spec.summary = "show an activity: metadata, metrics, zones, PMC, intervals and data series";
    show.spec.scope = Scope::Athlete;
    show.spec.params << ParamSpec("activity", ParamType::String, "activity id, start time, date, 'first' or 'last'").req().pos();
    show.spec.params << ParamSpec("imperial", ParamType::Bool, "metric values in imperial units");
    show.spec.params << ParamSpec("pmc-metric", ParamType::String, "stress metric for the performance manager values (default: govss for runs, swimscore for swims, else coggan_tss)");
    show.spec.httpMethod = "GET";
    show.spec.httpPath = "/athletes/{athlete}/activities/{activity}";
    show.handler = showActivity;
    registry.add(show);

    Command exportCmd;
    exportCmd.spec.name = "activity.export";
    exportCmd.spec.summary = "export an activity as json, csv, tcx, gpx, fit, pwx ...";
    exportCmd.spec.scope = Scope::Athlete;
    exportCmd.spec.params << ParamSpec("activity", ParamType::String, "activity id, start time, date, 'first' or 'last'").req().pos();
    exportCmd.spec.params << ParamSpec("as", ParamType::String, "file format (see 'formats'), or csv-gc / csv-wprime").def("json");
    exportCmd.spec.httpMethod = "GET";
    exportCmd.spec.httpPath = "/athletes/{athlete}/activities/{activity}/export";
    exportCmd.handler = exportActivity;
    registry.add(exportCmd);

    Command set;
    set.spec.name = "activity.set";
    set.spec.summary = "set metadata fields on activities and save them";
    set.spec.scope = Scope::Athlete;
    set.spec.modifies = true;
    set.spec.params << ActivitySelection::params(true);
    set.spec.params << ParamSpec("set", ParamType::String, "NAME=VALUE, an empty value removes the field").req().many();
    set.spec.params << ParamSpec("all", ParamType::Bool, "change every activity when nothing else is chosen");
    set.spec.params << ParamSpec("allow-undefined", ParamType::Bool, "allow fields that are not defined");
    set.spec.httpMethod = "PATCH";
    set.spec.httpPath = "/athletes/{athlete}/activities";
    set.handler = setFields;
    registry.add(set);

    Command remove;
    remove.spec.name = "activity.delete";
    remove.spec.summary = "delete activities (the files are kept in the athlete's backup folder)";
    remove.spec.scope = Scope::Athlete;
    remove.spec.modifies = true;
    remove.spec.params << ParamSpec("activity", ParamType::String, "activity id, start time or date").req().pos().many();
    remove.spec.httpMethod = "DELETE";
    remove.spec.httpPath = "/athletes/{athlete}/activities/{activity}";
    remove.handler = deleteActivities;
    registry.add(remove);

    Command eval;
    eval.spec.name = "activity.eval";
    eval.spec.summary = "evaluate a formula (the GUI's filter/metric language) for each activity";
    eval.spec.scope = Scope::Athlete;
    eval.spec.params << ParamSpec("expression", ParamType::String, "formula, e.g. 'Average_Power / Athlete_Weight'").req();
    eval.spec.params << ActivitySelection::params(true);
    eval.spec.httpMethod = "GET";
    eval.spec.httpPath = "/athletes/{athlete}/eval";
    eval.handler = evalActivities;
    registry.add(eval);
}

} // namespace Headless
