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
// Manual entry and planned activities, headless.
//
// 'activity add' and 'plan add' are the Manual entry and Plan activity
// wizard (ManualActivity in the core builds and saves the same file). The
// other plan commands are the calendar's actions on planned activities:
// RideCache's move, copy, link, unlink and shift, with the same checks, and
// saving the activities they change as the calendar does. Repeating,
// exporting and importing a plan go through RepeatPlan and PlanBundle, as
// the plan wizards do.
//

#include "HeadlessCommands.h"
#include "ActivitySelection.h"
#include "ActivityJson.h"
#include "MetricNames.h"
#include "ResultFormat.h"

#include "Context.h"
#include "Athlete.h"
#include "RideCache.h"
#include "RideItem.h"
#include "RideFile.h"
#include "RideMetric.h"
#include "Settings.h"
#include "Specification.h"
#include "ManualActivity.h"
#include "PlanBundle.h"
#include "TrainDB.h"
#include "ErgFile.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <cmath>
#include <climits>
#include <memory>

namespace Headless {

static QString
idOf(const RideItem *item)
{
    return QFileInfo(item->fileName).completeBaseName();
}

static QString
idOf(const QString &fileName)
{
    return QFileInfo(fileName).completeBaseName();
}

static QDate
dateArg(const CommandRequest &request, const QString &name)
{
    return QDate::fromString(request.args.value(name).toString(), Qt::ISODate);
}

// hh:mm or hh:mm:ss
static bool
parseTime(const QString &text, QTime &time)
{
    for (const char *format : { "H:mm:ss", "H:mm", "HH:mm:ss", "HH:mm" }) {
        time = QTime::fromString(text.trimmed(), format);
        if (time.isValid()) return true;
    }
    return false;
}

// seconds, or h:mm:ss
static bool
parseDuration(const QString &text, int &seconds)
{
    QString t = text.trimmed();
    bool ok = false;
    seconds = t.toInt(&ok);
    if (ok) return seconds >= 0;
    QStringList parts = t.split(':');
    if (parts.count() != 3) return false;
    int h = parts.at(0).toInt(&ok); if (!ok || h < 0) return false;
    int m = parts.at(1).toInt(&ok); if (!ok || m < 0 || m > 59 || parts.at(1).length() != 2) return false;
    int s = parts.at(2).toInt(&ok); if (!ok || s < 0 || s > 59 || parts.at(2).length() != 2) return false;
    seconds = h * 3600 + m * 60 + s;
    return true;
}

static QString
hms(int seconds)
{
    return QString("%1:%2:%3").arg(seconds / 3600).arg((seconds / 60) % 60, 2, 10, QChar('0'))
                              .arg(seconds % 60, 2, 10, QChar('0'));
}

// what a QSpinBox of the wizard ends up holding when it is set to v
static int
spinValue(double v, int max)
{
    if (!std::isfinite(v) || v <= 0) return 0;
    if (v >= max) return max;
    return qRound(v);
}

// the other activity of a link, as its id, or null
static QJsonValue
linkedJson(RideCache *cache, RideItem *item)
{
    if (!item->hasLinkedActivity()) return QJsonValue();
    RideItem *other = cache->getLinkedActivity(item);
    return other ? QJsonValue(idOf(other)) : QJsonValue(idOf(item->getLinkedFileName()));
}

// a planned activity as plan list shows it
static QJsonObject
plannedJson(RideCache *cache, RideItem *item)
{
    QJsonObject o;
    o.insert("id", idOf(item));
    o.insert("file", item->fileName);
    o.insert("date", item->dateTime.date().toString(Qt::ISODate));
    o.insert("time", item->dateTime.time().toString("HH:mm:ss"));
    o.insert("sport", item->sport);
    o.insert("subsport", item->getText("SubSport", ""));
    o.insert("title", item->getText("Route", ""));
    o.insert("workout_code", item->getText("Workout Code", ""));
    o.insert("duration", jsonNumber(item->getForSymbol("workout_time")));
    o.insert("distance", jsonNumber(item->getForSymbol("total_distance")));
    o.insert("coggan_tss", jsonNumber(item->getForSymbol("coggan_tss")));
    o.insert("linked", linkedJson(cache, item));
    QDate original = QDate::fromString(item->getText("Original Date", ""), "yyyy/MM/dd");
    o.insert("original_date", original.isValid() && original != item->dateTime.date()
                              ? QJsonValue(original.toString(Qt::ISODate)) : QJsonValue());
    QString workout = item->getText("WorkoutFilename", "");
    if (!workout.isEmpty()) o.insert("workout", workout);
    return o;
}

//
// activity add / plan add
//

// a workout of the train library, by file, file name or title
struct Workout {
    QString filename, title, type, description;
    int elevation = 0, durationMs = 0;
    double distance = 0;
};

static bool
findWorkout(const QString &given, Workout &found, QString &error)
{
    if (!trainDB) {
        error = "the workout library is not available";
        return false;
    }
    std::unique_ptr<QAbstractTableModel> model(trainDB->getWorkoutModel());
    QString path = QFileInfo(given).exists() ? QFileInfo(given).canonicalFilePath() : QString();
    QList<int> matches;
    for (int pass = 0; pass < 3 && matches.isEmpty(); pass++) {
        for (int row = 0; row < model->rowCount(); row++) {
            QString file = model->data(model->index(row, TdbWorkoutModelIdx::filepath)).toString();
            QString name = model->data(model->index(row, TdbWorkoutModelIdx::displayname)).toString().trimmed();
            bool hit = false;
            if (pass == 0) hit = file == given || (!path.isEmpty() && QFileInfo(file).canonicalFilePath() == path);
            if (pass == 1) hit = QFileInfo(file).fileName() == given || QFileInfo(file).completeBaseName() == given;
            if (pass == 2) hit = name.compare(given.trimmed(), Qt::CaseInsensitive) == 0;
            if (hit) matches << row;
        }
    }
    if (matches.isEmpty()) {
        error = QString("no workout '%1' in the workout library (the Train view's workouts)").arg(given);
        return false;
    }
    if (matches.count() > 1) {
        QStringList files;
        for (int row : matches) files << model->data(model->index(row, TdbWorkoutModelIdx::filepath)).toString();
        error = QString("%1 workouts are called '%2', give the file: %3").arg(matches.count()).arg(given).arg(files.join(", "));
        return false;
    }
    int row = matches.first();
    auto value = [&](int column) { return model->data(model->index(row, column), Qt::DisplayRole); };
    found.filename = value(TdbWorkoutModelIdx::filepath).toString();
    found.title = value(TdbWorkoutModelIdx::displayname).toString();
    found.type = value(TdbWorkoutModelIdx::type).toString();
    found.description = value(TdbWorkoutModelIdx::description).toString();
    found.elevation = value(TdbWorkoutModelIdx::elevation).toInt();
    found.durationMs = value(TdbWorkoutModelIdx::duration).toInt();
    found.distance = value(TdbWorkoutModelIdx::distance).toDouble();
    return true;
}

// the stress flags; giving any of them is entering stress by hand
static const QStringList stressFlags = { "work", "bikestress", "bikescore", "swimscore", "triscore" };

static CommandResult
addActivity(CommandEnvironment &env, const CommandRequest &request, bool plan)
{
    Context *context = env.session->context();
    RideCache *cache = env.session->rideCache();
    const QJsonObject &args = request.args;
    auto given = [&](const QString &name) { return args.contains(name); };

    ManualActivity activity;

    // when, as the wizard's first page: a plan from today, an activity until today
    QDate date = dateArg(request, "date");
    QDate today = QDate::currentDate();
    if (plan && date < today)
        return CommandResult::failure(Status::Usage, QString("a plan starts today or later, not on %1").arg(date.toString(Qt::ISODate)));
    if (!plan && (date < QDate(2000, 1, 1) || date > today))
        return CommandResult::failure(Status::Usage,
                    QString("an activity is dated from 2000-01-01 until today, not %1 (plan it with 'plan add')").arg(date.toString(Qt::ISODate)));
    QTime time;
    if (given("time")) {
        if (!parseTime(args.value("time").toString(), time))
            return CommandResult::failure(Status::Usage, QString("--time is hh:mm or hh:mm:ss, not '%1'").arg(args.value("time").toString()));
    } else if (plan) {
        time = QTime(16, 0, 0);     // Planned: 16:00 by default
    } else {
        QTime now = QTime::currentTime();
        time = QTime(now.hour(), now.minute(), now.second()).addSecs(-4 * 3600); // Completed: 4 hours ago by default
    }
    activity.start = QDateTime(date, time);

    // the workout, which decides the sport and what can be entered
    Workout workout;
    bool withWorkout = plan && given("workout");
    std::unique_ptr<ErgFile> ergFile;
    if (withWorkout) {
        QString error;
        if (!findWorkout(args.value("workout").toString(), workout, error)) return CommandResult::failure(Status::NotFound, error);
        if (given("sport") && RideFile::sportTag(args.value("sport").toString().trimmed()) != "Bike")
            return CommandResult::failure(Status::Usage, "a workout from the library is a Bike activity, --sport can't be changed");
        QStringList fromWorkout = { "title", "elevation-gain", "isopower", "xpower" };
        if (workout.type == "erg") fromWorkout << "duration" << "distance" << "avg-power" << "estimate" << stressFlags;
        else if (workout.type == "slp") fromWorkout << "distance";
        for (const QString &flag : fromWorkout)
            if (given(flag))
                return CommandResult::failure(Status::Usage, QString("--%1 comes from the %2 workout and can't be given with it").arg(flag).arg(workout.type));
        if (workout.type != "code") {
            ergFile.reset(new ErgFile(workout.filename, ErgFileFormat::unknown, context, date));
            if (!ergFile->isValid()) ergFile.reset();
        }
        activity.setWorkout(workout.filename, workout.title, workout.type, workout.description,
                            workout.elevation, workout.durationMs, workout.distance, ergFile.get());
        activity.sport = "Bike";
    } else {
        activity.sport = args.value("sport").toString().trimmed();
        if (activity.sport.isEmpty())
            return CommandResult::failure(Status::Usage, plan ? "give --sport, or a --workout from the library" : "give --sport (Bike, Run, Swim ...)");
        activity.workoutTitle = args.value("title").toString();
    }
    QString sport = RideFile::sportTag(activity.sport);

    activity.subSport = args.value("subsport").toString();
    activity.workoutCode = args.value("workout-code").toString();
    activity.notes = args.value("notes").toString();
    activity.objective = args.value("objective").toString();

    // numbers, within what the wizard's fields take
    struct Range { const char *flag; int max; int *target; };
    const QList<Range> ints = {
        { "rpe", 10, &activity.rpe },
        { "avg-hr", 250, &activity.averageHr },
        { "avg-cadence", 500, &activity.averageCadence },
        { "avg-power", 2000, &activity.averagePower },
        { "work", 9999, &activity.work },
        { "bikestress", 999, &activity.bikeStress },
        { "bikescore", 999, &activity.bikeScore },
        { "swimscore", 999, &activity.swimScore },
        { "triscore", 999, &activity.triScore },
        { "elevation-gain", 10000, &activity.elevationGain },
        { "isopower", 10000, &activity.isoPower },
        { "xpower", 10000, &activity.xPower },
    };
    for (const Range &r : ints) {
        if (!given(r.flag)) continue;
        int v = args.value(r.flag).toInt();
        if (v < 0 || v > r.max)
            return CommandResult::failure(Status::Usage, QString("--%1 is from 0 to %2, not %3").arg(r.flag).arg(r.max).arg(v));
        *r.target = v;
    }
    if (given("duration")) {
        int seconds = 0;
        if (!parseDuration(args.value("duration").toString(), seconds) || seconds >= 100000)
            return CommandResult::failure(Status::Usage, QString("--duration is h:mm:ss or seconds, less than 100000, not '%1'").arg(args.value("duration").toString()));
        activity.duration = seconds;
    }
    if (given("distance")) {
        double km = args.value("distance").toDouble();
        if (km < 0 || km >= 10000)
            return CommandResult::failure(Status::Usage, QString("--distance is in km, from 0 to 10000, not %1").arg(km));
        activity.distance = km;
    }

    // stress: estimated as the wizard estimates it, or entered by hand
    QString mode;
    if (given("estimate")) mode = args.value("estimate").toString();
    bool manualStress = false;
    for (const QString &flag : stressFlags) manualStress |= given(flag);
    if (manualStress && !mode.isEmpty() && mode != "none")
        return CommandResult::failure(Status::Usage, "--work and the stress values are entered by hand, they can't be given with --estimate time or distance");
    if (mode.isEmpty()) {
        QString setting = appsettings->value(nullptr, GC_BIKESCOREMODE).toString();
        mode = manualStress ? "none" : setting == "time" ? "time" : setting == "dist" ? "distance" : "none";
    }
    int days = appsettings->value(nullptr, GC_BIKESCOREDAYS, "30").toInt();
    if (given("estimate-days")) {
        days = args.value("estimate-days").toInt();
        if (days < 1 || days > 999) return CommandResult::failure(Status::Usage, QString("--estimate-days is from 1 to 999, not %1").arg(days));
    }
    bool ergWorkout = withWorkout && workout.type == "erg";  // no estimation for erg workouts
    if (ergWorkout) mode = "none";
    if (mode != "none") {
        ManualActivity::Estimate e = ManualActivity::estimate(context, sport, days,
                                        mode == "time" ? ManualActivity::EstimateBy::Duration : ManualActivity::EstimateBy::Distance,
                                        activity.duration, activity.distance);
        activity.work = spinValue(e.work, 9999);
        activity.bikeStress = spinValue(e.bikeStress, 999);
        activity.bikeScore = spinValue(e.bikeScore, 999);
        activity.swimScore = spinValue(e.swimScore, 999);
        activity.triScore = spinValue(e.triScore, 999);
    }

    // the wizard would overwrite an activity starting then, after a warning
    QString path = ManualActivity::fileName(context, activity.start, plan);
    for (RideItem *item : cache->rides())
        if (item->planned == plan && item->dateTime == activity.start)
            return CommandResult::failure(Status::Failed,
                        QString("%1 %2 already starts at %3").arg(plan ? "the planned activity" : "activity").arg(idOf(item))
                        .arg(activity.start.toString("yyyy-MM-dd HH:mm:ss")));
    if (QFile::exists(path))
        return CommandResult::failure(Status::Failed, QString("%1 already exists").arg(path));

    if (!activity.save(context, plan)) {
        QFile::remove(path);    // nothing of it may be left
        return CommandResult::failure(Status::Failed, QString("could not write %1").arg(path));
    }

    RideItem *item = cache->getRide(ManualActivity::baseName(activity.start) + ".json", plan);
    if (!item) return CommandResult::failure(Status::Internal, QString("%1 was written but not loaded").arg(path));

    QJsonObject data = activitySummary(item);
    if (plan) data.insert("planned", true);
    addMetadata(data, item, QStringList());
    QJsonObject overrides;
    if (RideFile *ride = item->ride()) {
        for (auto it = ride->metricOverrides.constBegin(); it != ride->metricOverrides.constEnd(); ++it) {
            bool ok = false;
            double v = it.value().value("value").toDouble(&ok);
            overrides.insert(it.key(), ok ? jsonNumber(v) : QJsonValue(it.value().value("value")));
        }
    }
    data.insert("overrides", overrides);
    QJsonObject estimate;
    estimate.insert("by", mode);
    if (mode != "none") estimate.insert("days", days);
    data.insert("estimate", estimate);

    CommandResult result = CommandResult::success(data);
    result.text = QString("%1 %2 (%3) %4\n").arg(plan ? "planned" : "added").arg(idOf(item))
                  .arg(item->sport.isEmpty() ? activity.sport : item->sport)
                  .arg(activity.start.toString("yyyy-MM-dd HH:mm:ss"));
    return result;
}

static CommandResult
addCompleted(CommandEnvironment &env, const CommandRequest &request)
{
    return addActivity(env, request, false);
}

static CommandResult
addPlanned(CommandEnvironment &env, const CommandRequest &request)
{
    return addActivity(env, request, true);
}

static QList<ParamSpec>
entryParams(bool plan)
{
    QList<ParamSpec> p;
    p << ParamSpec("date", ParamType::Date, plan ? "the day it is planned for, today or later" : "the day of the activity").req();
    p << ParamSpec("time", ParamType::String, plan ? "start time, hh:mm (default 16:00, as the wizard)"
                                                   : "start time, hh:mm (default: four hours ago, as the wizard)");
    p << ParamSpec("sport", ParamType::String, plan ? "Bike, Run, Swim ... (Bike with --workout)" : "Bike, Run, Swim ...");
    p << ParamSpec("subsport", ParamType::String, "sub sport");
    p << ParamSpec("workout-code", ParamType::String, "workout code");
    if (plan) {
        p << ParamSpec("objective", ParamType::String, "objective of the session");
        p << ParamSpec("workout", ParamType::String, "a workout of the workout library (Train view): its file, file name or title");
    } else {
        p << ParamSpec("rpe", ParamType::Int, "RPE, 0 to 10");
    }
    p << ParamSpec("notes", ParamType::String, "notes");
    p << ParamSpec("title", ParamType::String, "title (the Route field)");
    p << ParamSpec("duration", ParamType::String, "duration, h:mm:ss or seconds");
    p << ParamSpec("distance", ParamType::Double, "distance in km, for swims too (1.5 is 1500 m)");
    p << ParamSpec("avg-hr", ParamType::Int, "average heart rate, bpm");
    p << ParamSpec("avg-cadence", ParamType::Int, "average cadence, rpm");
    p << ParamSpec("avg-power", ParamType::Int, "average power, watts");
    p << ParamSpec("work", ParamType::Int, "work, kJ");
    p << ParamSpec("bikestress", ParamType::Int, "BikeStress (TSS)");
    p << ParamSpec("bikescore", ParamType::Int, "BikeScore");
    p << ParamSpec("swimscore", ParamType::Int, "SwimScore");
    p << ParamSpec("triscore", ParamType::Int, "TriScore");
    p << ParamSpec("elevation-gain", ParamType::Int, "elevation gain, metres");
    p << ParamSpec("isopower", ParamType::Int, "IsoPower (NP), watts");
    p << ParamSpec("xpower", ParamType::Int, "xPower, watts");
    p << ParamSpec("estimate", ParamType::String, "estimate work and stress from recent activities of the sport, by time or by distance "
                                                  "(default: as the wizard was last used, else none)").oneOf({ "time", "distance", "none" });
    p << ParamSpec("estimate-days", ParamType::Int, "the recent activities are those of the last N days (default: as the wizard, 30)");
    return p;
}

//
// plan list
//

static CommandResult
listPlan(CommandEnvironment &env, const CommandRequest &request)
{
    RideCache *cache = env.session->rideCache();
    bool all = request.args.value("all").toBool(false);
    QDate from = request.args.contains("from") ? dateArg(request, "from") : (all ? QDate() : QDate::currentDate());
    QDate to = request.args.contains("to") ? dateArg(request, "to") : QDate();
    QString sport = request.args.value("sport").toString();

    QStringList metrics;
    QString error;
    if (!resolveMetrics(splitList(request.args.value("metric")), metrics, error)) return CommandResult::failure(Status::Usage, error);

    QJsonArray list;
    QString text;
    for (RideItem *item : cache->rides()) {
        if (!item->planned) continue;
        QDate d = item->dateTime.date();
        if (from.isValid() && d < from) continue;
        if (to.isValid() && d > to) continue;
        if (!sport.isEmpty() && item->sport.compare(sport, Qt::CaseInsensitive) != 0) continue;
        QJsonObject o = plannedJson(cache, item);
        addMetrics(o, item, metrics);
        list.append(o);

        QString title = o.value("title").toString();
        QString code = o.value("workout_code").toString();
        QString name = title.isEmpty() ? code : (code.isEmpty() ? title : QString("%1 (%2)").arg(title, code));
        QStringList parts;
        parts << QString("%1 %2").arg(o.value("date").toString(), item->dateTime.time().toString("HH:mm"));
        parts << (item->sport.isEmpty() ? QString("-") : item->sport);
        if (!name.isEmpty()) parts << name;
        double seconds = item->getForSymbol("workout_time");
        if (seconds > 0) parts << hms(int(seconds));
        double km = item->getForSymbol("total_distance");
        if (km > 0) parts << QString("%1 km").arg(km, 0, 'f', 1);
        double tss = item->getForSymbol("coggan_tss");
        if (tss > 0) parts << QString("BikeStress %1").arg(tss, 0, 'f', 0);
        if (!o.value("linked").isNull()) parts << "done: " + o.value("linked").toString();
        if (!o.value("original_date").isNull()) parts << "planned for " + o.value("original_date").toString();
        text += parts.join("  ") + "\n";
    }
    QJsonObject data;
    data.insert("count", list.count());
    data.insert("from", from.isValid() ? QJsonValue(from.toString(Qt::ISODate)) : QJsonValue());
    data.insert("to", to.isValid() ? QJsonValue(to.toString(Qt::ISODate)) : QJsonValue());
    data.insert("planned", list);
    CommandResult result = CommandResult::success(data);
    result.text = text.isEmpty() ? QString("no planned activities\n") : text;
    return result;
}

//
// the calendar's actions
//

// a planned activity, by id
static RideItem *
findPlanned(CommandEnvironment &env, const QString &id, QString &error)
{
    RideItem *item = env.session->findActivity(id, error, true);
    if (item && !item->planned) {
        error = QString("%1 is a completed activity, not a planned one").arg(idOf(item));
        return nullptr;
    }
    return item;
}

// the check passes and nothing waits to be saved (there is nobody to ask)
static bool
checkPasses(const RideCache::OperationPreCheck &check, QString &error)
{
    if (!check.canProceed) {
        error = check.blockingReason;
        return false;
    }
    if (check.requiresUserDecision) {
        error = check.warningMessage;
        return false;
    }
    return true;
}

// save what the operation changed, as the calendar does after it
static bool
saveAffected(RideCache *cache, const QList<RideItem*> &items, QString &error)
{
    // an item that was renamed is still in the list
    QList<RideItem*> alive;
    for (RideItem *item : items) if (cache->rides().contains(item)) alive << item;
    return cache->saveActivities(alive, error);
}

static CommandResult
movePlanned(CommandEnvironment &env, const CommandRequest &request)
{
    QString error;
    RideItem *item = findPlanned(env, request.args.value("activity").toString(), error);
    if (!item) return CommandResult::failure(Status::NotFound, error);
    QTime time = item->dateTime.time();
    if (request.args.contains("time") && !parseTime(request.args.value("time").toString(), time))
        return CommandResult::failure(Status::Usage, QString("--time is hh:mm or hh:mm:ss, not '%1'").arg(request.args.value("time").toString()));
    QDateTime when(dateArg(request, "to"), time);

    RideCache *cache = env.session->rideCache();
    QString from = idOf(item);
    QDate fromDate = item->dateTime.date();
    RideCache::OperationPreCheck check = cache->checkMoveActivity(item, when);
    if (!checkPasses(check, error)) return CommandResult::failure(Status::Failed, error);
    RideCache::OperationResult result = cache->moveActivity(item, when);
    if (!result.success) return CommandResult::failure(Status::Failed, result.error);
    QString saveError;
    bool saved = saveAffected(cache, check.affectedItems, saveError);
    env.session->refresh();

    QJsonObject data = plannedJson(cache, item);
    data.insert("moved_from", from);
    CommandResult r = CommandResult::success(data);
    if (!saved) r.warnings << saveError;
    r.text = QString("moved %1 from %2 to %3 %4\n").arg(idOf(item)).arg(fromDate.toString(Qt::ISODate))
             .arg(when.date().toString(Qt::ISODate)).arg(when.time().toString("HH:mm:ss"));
    return r;
}

static CommandResult
copyPlanned(CommandEnvironment &env, const CommandRequest &request)
{
    QString error;
    RideItem *item = findPlanned(env, request.args.value("activity").toString(), error);
    if (!item) return CommandResult::failure(Status::NotFound, error);
    QTime time;
    if (request.args.contains("time") && !parseTime(request.args.value("time").toString(), time))
        return CommandResult::failure(Status::Usage, QString("--time is hh:mm or hh:mm:ss, not '%1'").arg(request.args.value("time").toString()));
    QDate day = dateArg(request, "to");
    QDateTime when(day, time.isValid() ? time : item->dateTime.time());

    RideCache *cache = env.session->rideCache();
    RideCache::OperationPreCheck check = cache->checkCopyPlannedActivity(item, day, time);
    if (!checkPasses(check, error)) return CommandResult::failure(Status::Failed, error);
    RideCache::OperationResult result = cache->copyPlannedActivity(item, day, time);
    if (!result.success) return CommandResult::failure(Status::Failed, result.error);
    QString saveError;
    bool saved = saveAffected(cache, check.affectedItems, saveError);
    env.session->refresh();

    RideItem *copy = cache->getRide(when.toString("yyyy_MM_dd_HH_mm_ss") + "." + QFileInfo(item->fileName).suffix(), true);
    if (!copy) return CommandResult::failure(Status::Internal, "the copy was made but not loaded");
    QJsonObject data = plannedJson(cache, copy);
    data.insert("copied_from", idOf(item));
    CommandResult r = CommandResult::success(data);
    if (!saved) r.warnings << saveError;
    r.text = QString("copied %1 to %2\n").arg(idOf(item)).arg(idOf(copy));
    return r;
}

static CommandResult
linkPlanned(CommandEnvironment &env, const CommandRequest &request)
{
    QString error;
    RideItem *planned = findPlanned(env, request.args.value("activity").toString(), error);
    if (!planned) return CommandResult::failure(Status::NotFound, error);
    RideItem *actual = env.session->findActivity(request.args.value("actual").toString(), error, false);
    if (!actual) return CommandResult::failure(Status::NotFound, error);

    RideCache *cache = env.session->rideCache();
    RideCache::OperationPreCheck check = cache->checkLinkActivities(planned, actual);
    if (!checkPasses(check, error)) return CommandResult::failure(Status::Failed, error);
    RideCache::OperationResult result = cache->linkActivities(planned, actual);
    if (!result.success) return CommandResult::failure(Status::Failed, result.error);
    if (!cache->saveActivities(check.affectedItems, error)) return CommandResult::failure(Status::Failed, error);
    env.session->refresh();

    QJsonObject data;
    data.insert("planned", idOf(planned));
    data.insert("actual", idOf(actual));
    CommandResult r = CommandResult::success(data);
    r.text = QString("linked planned %1 and %2\n").arg(idOf(planned)).arg(idOf(actual));
    return r;
}

static CommandResult
unlinkPlanned(CommandEnvironment &env, const CommandRequest &request)
{
    QString error;
    // either side of the link, the planned one first
    RideItem *item = env.session->findActivity(request.args.value("activity").toString(), error, true);
    if (!item) return CommandResult::failure(Status::NotFound, error);

    RideCache *cache = env.session->rideCache();
    RideCache::OperationPreCheck check = cache->checkUnlinkActivity(item);
    if (!checkPasses(check, error)) return CommandResult::failure(Status::Failed, QString("%1: %2").arg(idOf(item)).arg(error));
    RideItem *other = cache->getLinkedActivity(item);
    RideCache::OperationResult result = cache->unlinkActivity(item);
    if (!result.success) return CommandResult::failure(Status::Failed, result.error);
    if (!cache->saveActivities(check.affectedItems, error)) return CommandResult::failure(Status::Failed, error);
    env.session->refresh();

    RideItem *planned = item->planned ? item : other;
    RideItem *actual = item->planned ? other : item;
    QJsonObject data;
    data.insert("planned", idOf(planned));
    data.insert("actual", idOf(actual));
    CommandResult r = CommandResult::success(data);
    r.text = QString("unlinked planned %1 and %2\n").arg(idOf(planned)).arg(idOf(actual));
    return r;
}

static CommandResult
shiftPlanned(CommandEnvironment &env, const CommandRequest &request)
{
    QDate from = dateArg(request, "from");
    int days = request.args.value("days").toInt();
    if (days == 0) return CommandResult::failure(Status::Usage, "--days is a number of days other than 0, negative to move back");

    RideCache *cache = env.session->rideCache();
    QList<RideItem*> items;
    QDate earliest;
    for (RideItem *item : cache->rides()) {
        if (item->planned && item->dateTime.date() >= from) {
            items << item;
            if (!earliest.isValid() || item->dateTime.date() < earliest) earliest = item->dateTime.date();
        }
    }
    // as deleting a rest day: nothing moves to before --from
    int effective = days;
    if (days < 0 && earliest.isValid()) effective = std::max(days, -int(from.daysTo(earliest)));

    QString error;
    RideCache::OperationPreCheck check = cache->checkShiftPlannedActivities(from, days);
    if (!checkPasses(check, error)) return CommandResult::failure(Status::Failed, error);

    QList<QPair<RideItem*, QString>> before;
    for (RideItem *item : items) before << qMakePair(item, idOf(item));

    RideCache::OperationResult result = cache->shiftPlannedActivities(from, days);
    if (!result.success) return CommandResult::failure(Status::Failed, result.error);
    QString saveError;
    bool saved = saveAffected(cache, check.affectedItems, saveError);
    env.session->refresh();

    QJsonArray moved;
    for (const auto &p : before) {
        if (idOf(p.first) == p.second) continue;
        QJsonObject o;
        o.insert("from", p.second);
        o.insert("to", idOf(p.first));
        moved.append(o);
    }
    QJsonObject data;
    data.insert("from", from.toString(Qt::ISODate));
    data.insert("days", days);
    data.insert("shifted_by", items.isEmpty() ? 0 : effective);
    data.insert("moved", moved);
    data.insert("count", result.affectedCount);
    CommandResult r = result.error.isEmpty() ? CommandResult::success(data)
                                             : CommandResult::batch(data, items.count() - result.affectedCount, items.count(), result.error);
    if (!saved) r.warnings << saveError;
    r.text = QString("%1 planned activit%2 from %3 moved %4 day%5\n").arg(result.affectedCount)
             .arg(result.affectedCount == 1 ? "y" : "ies").arg(from.toString(Qt::ISODate))
             .arg(items.isEmpty() ? 0 : effective).arg(std::abs(effective) == 1 ? "" : "s");
    return r;
}

static CommandResult
repeatPlan(CommandEnvironment &env, const CommandRequest &request)
{
    QDate from = dateArg(request, "from"), to = dateArg(request, "to"), start = dateArg(request, "start");
    if (to < from) return CommandResult::failure(Status::Usage, "--to is before --from");
    if (start <= to) return CommandResult::failure(Status::Usage, "--start is after the period repeated (after --to)");
    bool keepGap = !request.args.value("no-gaps").toBool(false);
    bool preferOriginal = !request.args.value("current").toBool(false);

    RideCache *cache = env.session->rideCache();
    RepeatPlan repeat(env.session->context(), start);
    repeat.update(from, to, keepGap, preferOriginal);
    if (repeat.sourceRides.isEmpty())
        return CommandResult::failure(Status::Failed, QString("no planned activities from %1 to %2").arg(from.toString(Qt::ISODate), to.toString(Qt::ISODate)));

    QJsonArray copies, skipped, deleted;
    for (const SourceRide &s : repeat.sourceRides) {
        QJsonObject o;
        o.insert("source", idOf(s.rideItem));
        o.insert("date", s.targetDate.toString(Qt::ISODate));
        QDateTime when(s.targetDate, s.rideItem->dateTime.time());
        if (!s.selected) {
            o.insert("reason", "another planned activity of the period starts at the same day and time");
            skipped.append(o);
        } else if (s.targetBlocked) {
            o.insert("reason", "a linked planned activity starts then");
            skipped.append(o);
        } else {
            o.insert("id", when.toString("yyyy_MM_dd_HH_mm_ss"));
            copies.append(o);
        }
    }
    for (RideItem *item : repeat.getDeletionList()) deleted.append(idOf(item));
    QDate targetEnd = repeat.getTargetRangeEnd();

    RideCache::OperationPreCheck check;
    RideCache::OperationResult result;
    repeat.apply(check, result);
    env.session->refresh();
    if (!check.canProceed) return CommandResult::failure(Status::Failed, check.blockingReason);

    QJsonObject data;
    data.insert("from", start.toString(Qt::ISODate));
    data.insert("to", targetEnd.toString(Qt::ISODate));
    data.insert("copies", copies);
    data.insert("skipped", skipped);
    data.insert("deleted", deleted);
    data.insert("count", result.affectedCount);
    CommandResult r = result.error.isEmpty() ? CommandResult::success(data)
                                             : CommandResult::batch(data, copies.count() - result.affectedCount, copies.count(), result.error);
    if (copies.isEmpty()) r = CommandResult::failure(Status::Failed, "nothing to copy: every planned activity of the period was left out");
    r.text = QString("%1 planned activit%2 copied to %3 .. %4, %5 replaced, %6 left out\n")
             .arg(result.affectedCount).arg(result.affectedCount == 1 ? "y" : "ies")
             .arg(start.toString(Qt::ISODate), targetEnd.toString(Qt::ISODate)).arg(deleted.count()).arg(skipped.count());
    (void) cache;
    return r;
}

static CommandResult
exportPlan(CommandEnvironment &env, const CommandRequest &request)
{
    QDate from = dateArg(request, "from"), to = dateArg(request, "to");
    if (to < from) return CommandResult::failure(Status::Usage, "--to is before --from");
    Context *context = env.session->context();

    PlanExportDescription description;
    description.name = request.args.value("name").toString().trimmed();
    description.author = request.args.contains("author") ? request.args.value("author").toString().trimmed() : context->athlete->cyclist;
    description.copyright = request.args.value("copyright").toString().trimmed();
    description.description = request.args.value("description").toString().trimmed();
    description.preferOriginal = !request.args.value("current").toBool(false);
    description.rangeStart = from;
    description.rangeEnd = to;
    if (description.name.isEmpty()) return CommandResult::failure(Status::Usage, "--name can't be empty");
    if (description.author.isEmpty()) return CommandResult::failure(Status::Usage, "--author can't be empty");

    QList<SourceRide> rides = PlanBundle::sourceRides(context, from, to, description.preferOriginal);
    QStringList sports;
    QJsonArray activities;
    for (const SourceRide &s : rides) {
        if (!s.selected) continue;
        description.activityFiles << s.rideItem->fileName;
        QString sport = PlanBundle::getRideSport(s.rideItem);
        if (!sports.contains(sport)) sports << sport;
        activities.append(idOf(s.rideItem));
    }
    if (description.activityFiles.isEmpty())
        return CommandResult::failure(Status::Failed, QString("no planned activities from %1 to %2").arg(from.toString(Qt::ISODate), to.toString(Qt::ISODate)));
    sports.sort();
    description.sport = sports.join(", ");
    description.description = description.expandedDescription();

    QTemporaryDir tmp;
    QString fileName = PlanBundle::sanitizeFilename(description.name);
    if (fileName.isEmpty()) fileName = "plan";
    fileName += ".gcplan";
    description.planFile = tmp.filePath(fileName);
    if (!PlanBundle::exportBundle(context, description)) return CommandResult::failure(Status::Failed, "the plan could not be exported");
    QFile file(description.planFile);
    if (!file.open(QFile::ReadOnly)) return CommandResult::failure(Status::Failed, "the plan could not be exported");

    CommandResult result;
    result.payload = file.readAll();
    result.payloadName = fileName;
    result.payloadType = "application/zip";
    result.data.insert("name", description.name);
    result.data.insert("author", description.author);
    result.data.insert("sport", description.sport);
    result.data.insert("from", from.toString(Qt::ISODate));
    result.data.insert("to", to.toString(Qt::ISODate));
    result.data.insert("activities", activities);
    result.data.insert("bytes", result.payload.size());
    return result;
}

static CommandResult
importPlan(CommandEnvironment &env, const CommandRequest &request)
{
    QString path = request.args.value("file").toString();
    QDate start = dateArg(request, "start");
    RideCache *cache = env.session->rideCache();

    QSet<QString> before;
    for (RideItem *item : cache->rides()) if (item->planned) before.insert(item->fileName);

    PlanBundleReader reader(env.session->context(), start);
    PlanResult loaded = reader.loadBundle(path);
    if (!loaded.ok()) {
        CommandResult r = CommandResult::failure(Status::Failed, QString("%1 is not a valid plan: %2")
                                                 .arg(request.displayNames.value(path, path)).arg(loaded.errors.join("; ")));
        r.warnings = loaded.warnings;
        return r;
    }
    if (request.args.value("no-gap-days").toBool(false)) reader.setIncludeGapDays(false);
    QStringList replaced = reader.getActivitiesToRemove();
    PlanMetadata metadata = reader.getMetadata();
    QDate end = reader.getTargetRangeEnd();

    PlanResult imported = reader.importBundle();
    env.session->refresh();

    QJsonArray added, removed;
    for (RideItem *item : cache->rides())
        if (item->planned && !before.contains(item->fileName)) added.append(idOf(item));
    for (const QString &f : replaced) removed.append(idOf(f));

    QJsonObject data;
    data.insert("name", metadata.name);
    data.insert("author", metadata.author);
    data.insert("sport", metadata.sport);
    data.insert("from", start.toString(Qt::ISODate));
    data.insert("to", end.toString(Qt::ISODate));
    data.insert("imported", added);
    data.insert("replaced", removed);
    CommandResult r = imported.ok() ? CommandResult::success(data)
                                    : CommandResult::failure(Status::Failed, imported.errors.join("; "));
    if (!imported.ok()) r.data = data;
    r.warnings = loaded.warnings + imported.warnings;
    r.text = QString("imported %1 planned activit%2 of '%3', %4 .. %5, %6 replaced\n").arg(added.count())
             .arg(added.count() == 1 ? "y" : "ies").arg(metadata.name)
             .arg(start.toString(Qt::ISODate), end.toString(Qt::ISODate)).arg(removed.count());
    return r;
}

//
// calendar summary: the calendar's weekly totals
//

static CommandResult
calendarSummary(CommandEnvironment &env, const CommandRequest &request)
{
    QDate from = dateArg(request, "from"), to = dateArg(request, "to");
    if (to < from) return CommandResult::failure(Status::Usage, "--to is before --from");
    int days = request.args.value("days").toInt(7);
    if (days < 1) return CommandResult::failure(Status::Usage, "--days is 1 or more");

    QStringList symbols;
    QString error;
    QStringList asked = splitList(request.args.value("metric"));
    if (asked.isEmpty()) asked = QStringList { "ride_count", "total_distance", "coggan_tss", "workout_time" };
    if (!resolveMetrics(asked, symbols, error)) return CommandResult::failure(Status::Usage, error);

    static const QMap<QString, PlanFilterType> types = {
        { "always", PlanFilterType::IncludeAll },
        { "upcoming-or-missed", PlanFilterType::IncludeIfUpcomingOrMissed },
        { "upcoming", PlanFilterType::IncludeIfUpcoming },
        { "never", PlanFilterType::IncludeNone }
    };
    Specification spec;
    spec.setPlanFilter(types.value(request.args.value("planned").toString("always"), PlanFilterType::IncludeAll));
    bool metricUnits = !request.args.value("imperial").toBool(false);

    RideCache *cache = env.session->rideCache();
    QJsonArray buckets;
    for (QDate first = from; first <= to; first = first.addDays(days)) {
        QDate last = first.addDays(days - 1);
        spec.setDateRange(DateRange(first, last));
        QJsonObject o;
        o.insert("from", first.toString(Qt::ISODate));
        o.insert("to", last.toString(Qt::ISODate));
        QJsonObject values;
        for (const QString &symbol : symbols) {
            QString value = cache->getAggregate(symbol, spec, metricUnits, true);
            bool ok = false;
            double d = value.toDouble(&ok);
            values.insert(symbol, ok ? jsonNumber(d) : QJsonValue(value));
        }
        o.insert("metrics", values);
        buckets.append(o);
    }
    QJsonObject data;
    data.insert("days", days);
    data.insert("planned", request.args.value("planned").toString("always"));
    data.insert("summaries", buckets);
    return CommandResult::success(data);
}

void
registerPlanCommands(CommandRegistry &registry)
{
    Command add;
    add.spec.name = "activity.add";
    add.spec.summary = "add an activity by hand, as Activity > Manual entry";
    add.spec.description =
        "Writes the activity Activity > Manual entry would: the fields and the metrics\n"
        "given, set as overrides. Work and stress are estimated from the athlete's recent\n"
        "activities of the same sport, by time or distance, as the wizard does.";
    add.spec.scope = Scope::Athlete;
    add.spec.modifies = true;
    add.spec.params << entryParams(false);
    add.spec.httpMethod = "POST";
    add.spec.httpPath = "/athletes/{athlete}/activities";
    add.handler = addCompleted;
    registry.add(add);

    Command padd;
    padd.spec.name = "plan.add";
    padd.spec.summary = "plan an activity, as Activity > Plan activity";
    padd.spec.description =
        "Writes the planned activity Plan activity would, in the athlete's planned folder.\n"
        "With --workout the activity is that workout of the workout library: its title,\n"
        "description and, for erg workouts, duration, power and stress.";
    padd.spec.scope = Scope::Athlete;
    padd.spec.modifies = true;
    padd.spec.params << entryParams(true);
    padd.spec.httpMethod = "POST";
    padd.spec.httpPath = "/athletes/{athlete}/plan";
    padd.handler = addPlanned;
    registry.add(padd);

    Command list;
    list.spec.name = "plan.list";
    list.spec.summary = "list planned activities: the agenda";
    list.spec.scope = Scope::Athlete;
    list.spec.params << ParamSpec("from", ParamType::Date, "from this day (default today)");
    list.spec.params << ParamSpec("to", ParamType::Date, "until this day");
    list.spec.params << ParamSpec("all", ParamType::Bool, "past planned activities too");
    list.spec.params << ParamSpec("sport", ParamType::String, "only this sport");
    list.spec.params << ParamSpec("metric", ParamType::String, "more metrics, symbols or formula names (comma separated or repeated)").many();
    list.spec.httpMethod = "GET";
    list.spec.httpPath = "/athletes/{athlete}/plan";
    list.handler = listPlan;
    registry.add(list);

    Command move;
    move.spec.name = "plan.move";
    move.spec.summary = "move a planned activity to another day, as dragging it in the calendar";
    move.spec.scope = Scope::Athlete;
    move.spec.modifies = true;
    move.spec.params << ParamSpec("activity", ParamType::String, "planned activity: file name, start time or date").req().pos();
    move.spec.params << ParamSpec("to", ParamType::Date, "the new day").req();
    move.spec.params << ParamSpec("time", ParamType::String, "the new start time, hh:mm (default: the same)");
    move.spec.httpMethod = "POST";
    move.spec.httpPath = "/athletes/{athlete}/plan/{activity}/move";
    move.handler = movePlanned;
    registry.add(move);

    Command copy;
    copy.spec.name = "plan.copy";
    copy.spec.summary = "copy a planned activity to another day, as the calendar's copy and paste";
    copy.spec.scope = Scope::Athlete;
    copy.spec.modifies = true;
    copy.spec.params << ParamSpec("activity", ParamType::String, "planned activity: file name, start time or date").req().pos();
    copy.spec.params << ParamSpec("to", ParamType::Date, "the day of the copy").req();
    copy.spec.params << ParamSpec("time", ParamType::String, "the copy's start time, hh:mm (default: the same)");
    copy.spec.httpMethod = "POST";
    copy.spec.httpPath = "/athletes/{athlete}/plan/{activity}/copies";
    copy.handler = copyPlanned;
    registry.add(copy);

    Command link;
    link.spec.name = "plan.link";
    link.spec.summary = "link a planned activity to the completed activity that carried it out";
    link.spec.scope = Scope::Athlete;
    link.spec.modifies = true;
    link.spec.params << ParamSpec("activity", ParamType::String, "planned activity: file name, start time or date").req().pos();
    link.spec.params << ParamSpec("actual", ParamType::String, "completed activity: file name, start time, date or 'last'").req().pos();
    link.spec.httpMethod = "POST";
    link.spec.httpPath = "/athletes/{athlete}/plan/{activity}/link";
    link.handler = linkPlanned;
    registry.add(link);

    Command unlink;
    unlink.spec.name = "plan.unlink";
    unlink.spec.summary = "remove the link between a planned and a completed activity";
    unlink.spec.scope = Scope::Athlete;
    unlink.spec.modifies = true;
    unlink.spec.params << ParamSpec("activity", ParamType::String, "either activity of the link (a planned one if both have that name)").req().pos();
    unlink.spec.httpMethod = "DELETE";
    unlink.spec.httpPath = "/athletes/{athlete}/plan/{activity}/link";
    unlink.handler = unlinkPlanned;
    registry.add(unlink);

    Command shift;
    shift.spec.name = "plan.shift";
    shift.spec.summary = "move every planned activity from a day on by N days, as inserting or deleting rest days";
    shift.spec.description =
        "A positive --days inserts that many rest days, a negative one deletes them. As\n"
        "deleting a rest day in the calendar, nothing moves to before --from: the planned\n"
        "activities move back at most until the first of them is on --from.";
    shift.spec.scope = Scope::Athlete;
    shift.spec.modifies = true;
    shift.spec.params << ParamSpec("from", ParamType::Date, "the first day that moves").req();
    shift.spec.params << ParamSpec("days", ParamType::Int, "days to move, negative to move back").req();
    shift.spec.httpMethod = "POST";
    shift.spec.httpPath = "/athletes/{athlete}/plan/shift";
    shift.handler = shiftPlanned;
    registry.add(shift);

    Command repeat;
    repeat.spec.name = "plan.repeat";
    repeat.spec.summary = "repeat the planned activities of a period from another day on, as the Repeat Plan wizard";
    repeat.spec.description =
        "Copies the planned activities of --from .. --to to the same days counted from\n"
        "--start. Unlinked planned activities already in the new period are deleted, a\n"
        "copy that would start when a linked one does is left out, and of activities of\n"
        "the period starting on the same day and time only the first is copied.";
    repeat.spec.scope = Scope::Athlete;
    repeat.spec.modifies = true;
    repeat.spec.params << ParamSpec("from", ParamType::Date, "first day of the period to repeat").req();
    repeat.spec.params << ParamSpec("to", ParamType::Date, "last day of the period to repeat").req();
    repeat.spec.params << ParamSpec("start", ParamType::Date, "the day the repetition starts, after --to").req();
    repeat.spec.params << ParamSpec("current", ParamType::Bool, "as currently planned: take moved activities on their current day, not the day they were first planned for");
    repeat.spec.params << ParamSpec("no-gaps", ParamType::Bool, "drop the days before the first and after the last planned activity of the period");
    repeat.spec.httpMethod = "POST";
    repeat.spec.httpPath = "/athletes/{athlete}/plan/repeat";
    repeat.handler = repeatPlan;
    registry.add(repeat);

    Command exportCmd;
    exportCmd.spec.name = "plan.export";
    exportCmd.spec.summary = "export the planned activities of a period as a plan file, as Tools > Export Plan";
    exportCmd.spec.description =
        "Writes a .gcplan bundle with the planned activities of the period and the\n"
        "workouts they use, as the Export Plan wizard does. In the description, $NAME,\n"
        "$AUTHOR, $SPORT and $COPYRIGHT are filled in.";
    exportCmd.spec.scope = Scope::Athlete;
    exportCmd.spec.params << ParamSpec("from", ParamType::Date, "first day of the plan").req();
    exportCmd.spec.params << ParamSpec("to", ParamType::Date, "last day of the plan").req();
    exportCmd.spec.params << ParamSpec("name", ParamType::String, "the plan's name").req();
    exportCmd.spec.params << ParamSpec("author", ParamType::String, "the plan's author (default: the athlete's name)");
    exportCmd.spec.params << ParamSpec("copyright", ParamType::String, "copyright");
    exportCmd.spec.params << ParamSpec("description", ParamType::String, "description, Markdown");
    exportCmd.spec.params << ParamSpec("current", ParamType::Bool, "as currently planned: take moved activities on their current day");
    exportCmd.spec.httpMethod = "GET";
    exportCmd.spec.httpPath = "/athletes/{athlete}/plan/export";
    exportCmd.handler = exportPlan;
    registry.add(exportCmd);

    Command importCmd;
    importCmd.spec.name = "plan.import";
    importCmd.spec.summary = "import a plan file from a day on, as the Import Plan wizard";
    importCmd.spec.description =
        "Adds the plan's activities, and the workouts they use to the workout library.\n"
        "Unlinked planned activities already in the plan's period are deleted.";
    importCmd.spec.scope = Scope::Athlete;
    importCmd.spec.modifies = true;
    importCmd.spec.params << ParamSpec("file", ParamType::Path, "the .gcplan file").req().pos().upload();
    importCmd.spec.params << ParamSpec("start", ParamType::Date, "the day the plan starts").req();
    importCmd.spec.params << ParamSpec("no-gap-days", ParamType::Bool, "start with the plan's first activity, dropping the days before it and after the last");
    importCmd.spec.httpMethod = "POST";
    importCmd.spec.httpPath = "/athletes/{athlete}/plan/imports";
    importCmd.handler = importPlan;
    registry.add(importCmd);

    Command summary;
    summary.spec.name = "calendar.summary";
    summary.spec.summary = "the calendar's totals per week (or per N days), planned activities included";
    summary.spec.description =
        "The numbers the calendar's summary column shows: by default activities, distance,\n"
        "BikeStress and duration, per week from --from. Start --from on the first day of\n"
        "your calendar week to get the calendar's weeks.";
    summary.spec.scope = Scope::Athlete;
    summary.spec.params << ParamSpec("from", ParamType::Date, "the first day").req();
    summary.spec.params << ParamSpec("to", ParamType::Date, "the last day").req();
    summary.spec.params << ParamSpec("days", ParamType::Int, "days per total (default 7)").def(7);
    summary.spec.params << ParamSpec("planned", ParamType::String, "planned activities counted: always, upcoming-or-missed (not linked), upcoming (not linked, from today) or never, as the calendar's Include Planned")
                            .oneOf({ "always", "upcoming-or-missed", "upcoming", "never" }).def("always");
    summary.spec.params << ParamSpec("metric", ParamType::String, "metrics (default ride_count, total_distance, coggan_tss, workout_time)").many();
    summary.spec.params << ParamSpec("imperial", ParamType::Bool, "imperial units");
    summary.spec.httpMethod = "GET";
    summary.spec.httpPath = "/athletes/{athlete}/calendar/summary";
    summary.handler = calendarSummary;
    registry.add(summary);
}

} // namespace Headless
