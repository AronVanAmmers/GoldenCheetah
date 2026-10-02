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
#include "MetricNames.h"
#include "ActivityJson.h"
#include "IntervalData.h"
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
#include "SpecialFields.h"
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

    QStringList metrics;
    if (!resolveMetrics(splitList(request.args.value("metric")), metrics, error)) return CommandResult::failure(Status::Usage, error);
    QStringList fields = splitList(request.args.value("field"));
    bool metricUnits = !request.args.value("imperial").toBool(false);

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
    RideItem *item = env.session->findActivity(request.args.value("activity").toString(), error,
                                               request.args.value("planned").toBool(false));
    if (!item) return CommandResult::failure(Status::NotFound, error);
    bool metricUnits = !request.args.value("imperial").toBool(false);

    // the PMC tile's metric, checked before any work is done
    // the default overview tiles use GOVSS for runs and SwimScore for swims
    QString pmcMetric = request.args.value("pmc-metric").toString();
    if (pmcMetric.isEmpty()) pmcMetric = item->isRun ? "govss" : item->isSwim ? "swimscore" : "coggan_tss";
    QString pmcSymbol = metricSymbol(pmcMetric);
    if (!pmcSymbol.isEmpty()) pmcMetric = pmcSymbol;
    if (!RideMetricFactory::instance().haveMetric(pmcMetric))
        return CommandResult::failure(Status::Usage, QString("unknown metric '%1', see 'metric list'").arg(pmcMetric));

    QJsonObject o = activitySummary(item);
    addMetadata(o, item, QStringList());

    // every metric relevant for the activity, zeros too as the GUI shows them
    const RideMetricFactory &factory = RideMetricFactory::instance();
    QJsonObject metrics;
    for (int i = 0; i < factory.metricCount(); i++) {
        const RideMetric *m = factory.rideMetric(factory.metricName(i));
        if (!m || !m->isRelevantForRide(item)) continue;
        metrics.insert(m->symbol(), jsonNumber(item->getForSymbol(m->symbol(), metricUnits)));
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
    PMCData *pmc = pmcFor(*env.session, pmcMetric);
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

    // in brief, 'interval list' and 'interval show' have their metrics and
    // 'activity overview' the interval tables as the GUI shows them
    QJsonArray intervals;
    int number = 0;
    for (IntervalItem *i : item->intervals()) {
        QJsonObject io = intervalJson(i, ++number);
        io.insert("distance", jsonNumber(i->getForSymbol("total_distance", metricUnits)));
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
    RideItem *item = env.session->findActivity(request.args.value("activity").toString(), error,
                                               request.args.value("planned").toBool(false));
    if (!item) return CommandResult::failure(Status::NotFound, error);

    // the format first, before the whole file is read
    QString format = request.args.value("as").toString().toLower();
    const RideFileFactory &factory = RideFileFactory::instance();
    bool special = format == "csv-gc" || format == "csv-wprime";
    if (!special && !factory.writeSuffixes().contains(format))
        return CommandResult::failure(Status::Usage,
                    QString("can't export as '%1', choose one of: %2, csv-gc, csv-wprime")
                    .arg(format).arg(factory.writeSuffixes().join(", ")));

    RideFile *ride = item->ride();
    if (!ride) return CommandResult::failure(Status::Failed, "can't open the activity file");

    QTemporaryDir tmp;
    QString suffix = special ? QString("csv") : format;
    QString path = tmp.filePath("export." + suffix);
    QFile file(path);
    bool ok;
    if (special) {
        CsvFileReader writer;
        ok = writer.writeRideFile(env.session->context(), ride, file, format == "csv-gc" ? CsvFileReader::gc : CsvFileReader::wprime);
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

// a value as typed on the command line, in the form the Details tab stores
// it: dates as dd/MM/yyyy, times of day as hh:mm:ss.zzz, a checkbox as 0 or 1
static bool
storedText(const FieldDefinition &field, const QString &value, QString &text, QString &error)
{
    text = value;
    QString v = value.trimmed();
    bool isMetric = SpecialFields::getInstance().isMetric(field.name);
    bool date = field.name == "Start Date" || (!isMetric && field.type == GcFieldType::FIELD_DATE);
    bool time = field.name == "Start Time" || (!isMetric && field.type == GcFieldType::FIELD_TIME);

    if (field.name == "Summary") {
        error = "'Summary' is computed, it can't be set";
        return false;
    }
    if (field.interval) {
        error = QString("'%1' is a field of intervals, not of the activity").arg(field.name);
        return false;
    }
    if (v.isEmpty()) {
        if (date || time || field.name == "Recording Interval") {
            error = QString("'%1' can't be empty").arg(field.name);
            return false;
        }
        return true;
    }

    if (date) {
        QDate d = QDate::fromString(v, Qt::ISODate);
        if (!d.isValid()) d = QDate::fromString(v, "dd/MM/yyyy");
        if (!d.isValid()) {
            error = QString("'%1' is a date (yyyy-mm-dd), not '%2'").arg(field.name, value);
            return false;
        }
        text = d.toString("dd/MM/yyyy");
    } else if (time) {
        QTime t;
        for (const char *format : { "hh:mm:ss.zzz", "hh:mm:ss", "hh:mm", "h:mm:ss", "h:mm" }) {
            t = QTime::fromString(v, format);
            if (t.isValid()) break;
        }
        if (!t.isValid()) {
            error = QString("'%1' is a time of day (hh:mm:ss), not '%2'").arg(field.name, value);
            return false;
        }
        text = t.toString("hh:mm:ss.zzz");
    } else if (field.type == GcFieldType::FIELD_CHECKBOX && !isMetric) {
        QString b = v.toLower();
        if (b == "1" || b == "true" || b == "yes" || b == "on") text = "1";
        else if (b == "0" || b == "false" || b == "no" || b == "off") text = "0";
        else {
            error = QString("'%1' is a checkbox, give 1 or 0, not '%2'").arg(field.name, value);
            return false;
        }
    } else if (isMetric || field.name == "Recording Interval" || field.type == GcFieldType::FIELD_INTEGER
               || field.type == GcFieldType::FIELD_DOUBLE) {
        bool ok = false;
        v.toDouble(&ok);
        if (!ok) {
            error = QString("field '%1' is numeric, '%2' is not a number").arg(field.name, value);
            return false;
        }
    }
    return true;
}

// another activity already starts then: saving would give this one its file name
static bool
startTaken(RideCache *cache, const RideItem *item)
{
    // planned activities have a folder of their own
    for (const RideItem *other : cache->rides())
        if (other != item && other->planned == item->planned && other->dateTime == item->dateTime) return true;
    return false;
}

static CommandResult
setFields(CommandEnvironment &env, const CommandRequest &request)
{
    ActivitySelection selection = ActivitySelection::fromArgs(request.args);
    QString why = selection.requireExplicit();
    if (!why.isEmpty()) return CommandResult::failure(Status::Usage, why);

    // NAME=VALUE pairs, checked before any activity is touched
    struct Assignment { FieldDefinition field; QString text; };
    QList<Assignment> assignments;
    QMap<QString, FieldDefinition> defs;
    RideMetadata *metadata = GlobalContext::context()->rideMetadata;
    for (const FieldDefinition &f : metadata->getFields()) defs.insert(f.name, f);

    QList<QPair<QString, QString>> pairs;
    QString bad;
    if (!parseAssignments(request.args.value("set"), pairs, bad))
        return CommandResult::failure(Status::Usage, QString("expected NAME=VALUE, got '%1'").arg(bad));
    for (const auto &pair : pairs) {
        const QString &name = pair.first, &value = pair.second;

        if (!defs.contains(name) && !request.args.value("allow-undefined").toBool(false))
            return CommandResult::failure(Status::Usage,
                        QString("'%1' is not a defined field, add it with 'field add' or pass --allow-undefined").arg(name));
        Assignment a;
        if (defs.contains(name)) a.field = defs.value(name);
        else {
            a.field.name = name;
            a.field.type = GcFieldType::FIELD_TEXT;
        }
        QString error;
        if (!storedText(a.field, value, a.text, error)) return CommandResult::failure(Status::Usage, error);
        assignments << a;
    }

    QList<RideItem *> items;
    QString error;
    Status status;
    if (!selection.resolve(*env.session, items, error, status)) return CommandResult::failure(status, error);

    RideCache *cache = env.session->rideCache();
    int updated = 0, failed = 0;
    QJsonArray report;
    for (RideItem *item : items) {
        // an activity opened here is closed again once it is saved, so
        // --all doesn't hold every activity's samples in memory at once
        bool wasOpen = item->isOpen();
        QJsonObject r;
        r.insert("activity", QFileInfo(item->fileName).completeBaseName());
        if (!item->ride()) {
            r.insert("status", "failed");
            r.insert("message", QString("can't open the activity file: %1").arg(item->errors().join("; ")));
            report.append(r);
            failed++;
            continue;
        }

        // as the Details tab edits a field: special fields, metric
        // overrides, tags, and the fields linked to the value
        QDateTime start = item->dateTime;
        bool any = false;
        QString why;
        for (const Assignment &a : assignments) {
            QString text = a.text;
            QString error;
            bool changed = RideMetadata::applyFieldValue(item, nullptr, a.field, text, metadata->getDefaults(), true, &error);
            if (!error.isEmpty()) {
                why = error;
                break;
            }
            any |= changed;
        }
        if (why.isEmpty() && item->dateTime != start && startTaken(cache, item))
            why = QString("another activity starts at %1").arg(item->dateTime.toString("yyyy-MM-dd hh:mm:ss"));

        if (!why.isEmpty()) {
            // throw the change away
            if (item->dateTime != start) item->setStartTime(start);
            item->setDirty(false);
            item->close();
            r.insert("status", "failed");
            r.insert("message", why);
            report.append(r);
            failed++;
            continue;
        }

        r.insert("status", any ? "updated" : "unchanged");
        if (any) {
            item->notifyRideMetadataChanged();
            item->setDirty(true);
            QString saveError;
            if (cache->saveActivity(item, saveError)) {
                updated++;
                // the file is named after the start
                if (item->dateTime != start) r.insert("renamed", QFileInfo(item->fileName).completeBaseName());
            } else {
                r.insert("status", "failed");
                r.insert("message", saveError);
                if (item->dateTime != start) item->setStartTime(start);
                item->setDirty(false);  // the change is thrown away
                failed++;
            }
        }
        report.append(r);
        if (!wasOpen) item->close();
    }

    if (updated) env.session->refresh();

    QJsonObject data;
    data.insert("activities", report);
    data.insert("updated", updated);
    data.insert("failed", failed);
    return CommandResult::batch(data, failed, items.count(),
                                QString("%1 activit%2 not updated").arg(failed).arg(failed == 1 ? "y was" : "ies were"));
}

static CommandResult
deleteActivities(CommandEnvironment &env, const CommandRequest &request)
{
    QStringList ids;
    for (const QJsonValue &v : request.args.value("activity").toArray()) ids << v.toString();

    QStringList files, paths;
    bool planned = request.args.value("planned").toBool(false);
    ActivityLookup lookup(env.session->rideCache(), planned);
    for (const QString &id : ids) {
        QString error;
        RideItem *item = lookup.find(id, error);
        if (!item) return CommandResult::failure(Status::NotFound, error);
        if (files.contains(item->fileName)) continue;
        // the ride cache deletes by file name
        if (otherKindNamed(env.session->rideCache(), item->fileName, item->planned))
                return CommandResult::failure(Status::Failed,
                            QString("a planned and a completed activity are both called %1, and GoldenCheetah "
                                    "can't tell them apart when deleting; move the planned one first ('plan move')")
                            .arg(QFileInfo(item->fileName).completeBaseName()));
        files << item->fileName;
        paths << QDir(item->path).absoluteFilePath(item->fileName);
    }

    // one refresh for all of them. A file that couldn't be moved to the
    // backup folder is still there (the ride cache only logs that)
    env.session->rideCache()->removeRides(files);

    QJsonArray deleted, failed;
    for (int i = 0; i < files.count(); i++) {
        QString id = QFileInfo(files.at(i)).completeBaseName();
        if (QFile::exists(paths.at(i))) failed.append(id);
        else deleted.append(id);
    }
    QJsonObject data;
    data.insert("deleted", deleted);
    data.insert("failed", failed);
    data.insert("backup", "activities are moved to the athlete's bak folder");
    if (planned) data.insert("planned", true);
    return CommandResult::batch(data, failed.count(), files.count(),
                                QString("%1 activit%2 could not be moved to the bak folder")
                                .arg(failed.count()).arg(failed.count() == 1 ? "y" : "ies"));
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
            o.insert("value", jsonNumber(v));
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
    list.spec.params << ParamSpec("metric", ParamType::String, "metric symbols or formula names, e.g. Average_Power (comma separated or repeated)").many();
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
    show.spec.params << ParamSpec("planned", ParamType::Bool, "a planned activity: dates, times, 'first' and 'last' are planned ones");
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
    exportCmd.spec.params << ParamSpec("planned", ParamType::Bool, "a planned activity: dates, times, 'first' and 'last' are planned ones");
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
    set.spec.params << ParamSpec("set", ParamType::String, "NAME=VALUE, as the Details tab edits the field; an empty value clears it").req().many();
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
    remove.spec.params << ParamSpec("planned", ParamType::Bool, "planned activities: dates and times are planned ones");
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
