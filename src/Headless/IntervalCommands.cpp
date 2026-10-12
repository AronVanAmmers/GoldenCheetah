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
// Intervals of an activity: laps, the entire activity and the efforts,
// climbs and segments the GUI discovers, with their metrics as the
// intervals sidebar and the interval tables show them. The laps and marked
// intervals are changed as the sidebar changes them: added, edited, deleted,
// renamed, sorted, and merged.
//

#include "HeadlessCommands.h"
#include "ActivitySelection.h"
#include "MetricNames.h"
#include "ActivityJson.h"
#include "IntervalData.h"
#include "ResultFormat.h"

#include "Settings.h"
#include "RideCache.h"
#include "RideItem.h"
#include "RideFile.h"
#include "IntervalItem.h"
#include "RideMetric.h"
#include "TimeUtils.h"

#include <QColor>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace Headless {


// the metrics the intervals sidebar shows, as set in the GUI's preferences
static QStringList
summaryMetrics(RideItem *item)
{
    QStringList symbols;
    const RideMetricFactory &factory = RideMetricFactory::instance();
    for (const QString &symbol : favouriteMetrics()) {
        const RideMetric *m = factory.rideMetric(symbol);
        if (m && m->isRelevantForRide(item)) symbols << m->symbol();
    }
    return symbols;
}

// metric values, every one relevant for the activity with all; as
// numbers, or with display as the GUI formats them ("51:51")
static QJsonObject
intervalMetrics(IntervalItem *interval, QStringList symbols, bool all, bool metricUnits, bool display)
{
    const RideMetricFactory &factory = RideMetricFactory::instance();
    QJsonObject m;
    // metrics are computed when the ride cache refreshes, none means not yet
    if (interval->metrics().size() != factory.metricCount()) return m;

    if (all) {
        for (int i = 0; i < factory.metricCount(); i++) {
            const RideMetric *metric = factory.rideMetric(factory.metricName(i));
            if (metric && interval->rideItem() && metric->isRelevantForRide(interval->rideItem())) symbols << metric->symbol();
        }
    }
    for (const QString &symbol : symbols) {
        if (display) m.insert(symbol, interval->getStringForSymbol(symbol, metricUnits));
        else m.insert(symbol, jsonNumber(interval->getForSymbol(symbol, metricUnits)));
    }
    return m;
}

static CommandResult
listIntervals(CommandEnvironment &env, const CommandRequest &request)
{
    QString error;
    RideItem *item = env.session->findActivity(request.args.value("activity").toString(), error);
    if (!item) return CommandResult::failure(Status::NotFound, error);

    bool metricUnits = !request.args.value("imperial").toBool(false);
    bool display = request.args.value("display").toBool(false);
    QStringList symbols;
    if (!resolveMetrics(splitList(request.args.value("metric")), symbols, error)) return CommandResult::failure(Status::Usage, error);
    // the intervals sidebar's metrics, unless asked for others (none when
    // the favourites are set to none, as in the GUI)
    if (symbols.isEmpty()) symbols = summaryMetrics(item);

    QList<RideFileInterval::IntervalType> types;
    for (const QString &t : splitList(request.args.value("type"))) {
        RideFileInterval::IntervalType type;
        if (!intervalTypeFromName(t, type, error)) return CommandResult::failure(Status::Usage, error);
        types << type;
    }

    QJsonArray list;
    int number = 0;
    for (IntervalItem *interval : item->intervals()) {
        number++;
        if (!types.isEmpty() && !types.contains(interval->type)) continue;
        QJsonObject o = intervalJson(interval, number);
        o.insert("metrics", intervalMetrics(interval, symbols, false, metricUnits, display));
        list.append(o);
    }
    IntervalCensus census = intervalCensus(item);
    QJsonObject data;
    data.insert("activity", QFileInfo(item->fileName).completeBaseName());
    data.insert("user_intervals", census.userIntervals);
    data.insert("discovered_efforts", census.discoveredEfforts);
    data.insert("intervals", list);

    // the counts are for the whole activity, so they stay when --type
    // filters the table. The text report leads with that one line.
    // CSV is the table, starting at the header.
    QJsonObject table = data;
    table.remove("user_intervals");
    table.remove("discovered_efforts");
    CommandResult result = CommandResult::success(data);
    QString line = intervalCensusLine(census);
    // metric columns follow the favourites, or --metric, rather than sorted keys
    result.text = line + "\n" + ResultFormat::render(table, symbols);
    result.csv = ResultFormat::csv(table, symbols);
    return result;
}

// intervals by number as interval list shows them, a range of numbers (2-4)
// or a name; each one once, in the order given
static bool
findIntervals(RideItem *item, const QStringList &ids, QList<int> &numbers, Status &status, QString &error)
{
    static const QRegularExpression range(QRegularExpression::anchoredPattern("(\\d+)\\s*-\\s*(\\d+)"));
    const QList<IntervalItem*> &intervals = item->intervals();
    QString activity = QFileInfo(item->fileName).completeBaseName();
    numbers.clear();

    for (const QString &given : ids) {
        QString id = given.trimmed();
        QList<int> matches;
        bool isNumber = false;
        int n = id.toInt(&isNumber);
        if (isNumber) {
            if (n >= 1 && n <= intervals.count()) matches << n;
        } else {
            for (int i = 0; i < intervals.count(); i++)
                if (intervals[i]->name.compare(id, Qt::CaseInsensitive) == 0) matches << i + 1;

            // a range, unless an interval is called that
            QRegularExpressionMatch r = range.match(id);
            if (matches.isEmpty() && r.hasMatch()) {
                int from = r.captured(1).toInt(), to = r.captured(2).toInt();
                if (from < 1 || from > to || to > intervals.count()) {
                    status = Status::NotFound;
                    error = QString("no intervals %1 in %2, it has %3, see 'interval list'").arg(id, activity).arg(intervals.count());
                    return false;
                }
                for (int k = from; k <= to; k++) if (!numbers.contains(k)) numbers << k;
                continue;
            }
        }
        if (matches.isEmpty()) {
            status = Status::NotFound;
            error = QString("no interval '%1' in %2, see 'interval list'").arg(id, activity);
            return false;
        }
        if (matches.count() > 1) {
            QStringList list;
            for (int m : matches) list << QString::number(m);
            status = Status::Usage;
            error = QString("more than one interval is called '%1', choose one by number (%2)").arg(id, list.join(", "));
            return false;
        }
        if (!numbers.contains(matches.first())) numbers << matches.first();
    }
    if (numbers.isEmpty()) {
        status = Status::Usage;
        error = "no interval given, see 'interval list'";
        return false;
    }
    return true;
}

// seconds from the start of the activity: 754, 754.5, 12:34 or 1:02:03
static bool
parseTime(const QString &text, double &secs)
{
    QStringList parts = text.trimmed().split(':');
    if (parts.count() > 3) return false;
    secs = 0;
    for (int i = 0; i < parts.count(); i++) {
        bool ok = false;
        double v = parts.at(i).trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(v) || v < 0) return false;
        bool last = i == parts.count() - 1;
        if (!last && v != std::floor(v)) return false;
        if (i > 0 && v >= 60) return false;
        secs = secs * 60 + v;
    }
    return true;
}

static QString
timeText(double secs)
{
    return time_to_string(secs, true);
}

//
// Changing the laps and marked intervals of an activity: the intervals the
// intervals sidebar lets you edit, which are saved in the activity file.
// Discovered intervals (efforts, peaks, climbs, segments) and the entire
// activity are found again on every refresh, so they can't be changed.
//
class IntervalChange
{
    public:

        // finds the activity and opens it, see failure when it can't
        IntervalChange(CommandEnvironment &env, const CommandRequest &request) : env(env)
        {
            QString error;
            item = env.session->findActivity(request.args.value("activity").toString(), error);
            if (!item) { failure = CommandResult::failure(Status::NotFound, error); return; }
            id = QFileInfo(item->fileName).completeBaseName();
            dryRun = request.args.value("dry-run").toBool(false);

            // opened before the intervals are looked up: opening an activity
            // that needs a refresh makes its interval list again
            wasOpen = item->isOpen();
            ride = item->ride();
            if (!ride) { fail(Status::Failed, QString("can't open the activity file: %1").arg(item->errors().join("; "))); return; }
            if (ride->dataPoints().isEmpty()) { fail(Status::Failed, "the activity has no samples"); return; }
        }

        ~IntervalChange() { if (item && !wasOpen) item->close(); }

        bool ok() const { return ride != nullptr; }

        // a change made but not saved is thrown away: the intervals are read
        // again from the file
        CommandResult fail(Status status, const QString &why, bool named = false)
        {
            if (item && item->isDirty()) {
                item->setDirty(false);
                item->close();
                wasOpen = true;
                item->isstale = true;
                env.session->refresh();
            }
            ride = nullptr;
            failure = CommandResult::failure(status, named ? why : QString("%1: %2").arg(id, why));
            return failure;
        }

        // intervals by number or name; with user set, only laps and marked intervals
        bool find(const QJsonValue &ids, bool user, QList<int> &numbers, QList<IntervalItem*> &found)
        {
            QString error;
            Status status;
            if (!findIntervals(item, splitList(ids), numbers, status, error)) { fail(status, error, true); return false; }
            found.clear();
            for (int n : numbers) {
                IntervalItem *interval = item->intervals().at(n - 1);
                if (user && interval->type != RideFileInterval::USER) {
                    fail(Status::Usage, QString("interval %1 '%2' is %3: only laps and marked intervals (type user) can be "
                                                "changed, the others are found again on every refresh")
                                        .arg(n).arg(interval->name, RideFileInterval::typeDescription(interval->type)));
                    return false;
                }
                if (user && !interval->rideInterval) {
                    fail(Status::Failed, QString("interval %1 '%2' isn't in the activity file, run 'athlete refresh' and try again")
                                         .arg(n).arg(interval->name));
                    return false;
                }
                found << interval;
            }
            return true;
        }

        // a span within the activity, and short of all of it
        bool checkSpan(double start, double stop, const QString &instead = QString())
        {
            double end = ride->dataPoints().last()->secs + ride->recIntSecs();
            if (stop <= start) {
                fail(Status::Usage, QString("the interval would end (%1) before it starts (%2)").arg(timeText(stop), timeText(start)));
                return false;
            }
            if (stop > end + 1e-6) {
                fail(Status::Usage, QString("the interval would end at %1, after the activity does (%2)").arg(timeText(stop), timeText(end)));
                return false;
            }
            if (item->isEntireActivity(start, stop)) {
                fail(Status::Usage, QString("an interval from %1 to %2 is the whole activity: GoldenCheetah lists that as "
                                            "'Entire Activity' and hides a lap over the same span, so it isn't saved%3")
                                    .arg(timeText(start), timeText(stop), instead));
                return false;
            }
            return true;
        }

        // the change saved as the GUI saves an activity, and the metrics
        // recomputed; a change that can't be saved is thrown away. The
        // interval given is found again after the save, see saved()
        bool save(const IntervalItem *changed = nullptr)
        {
            // which of the user intervals with its name and span it is: they
            // are listed in the order of the file, and a new one is last in both
            if (changed) {
                tracked = { changed->name, changed->start, changed->stop };
                occurrence = 0;
                for (const IntervalItem *other : item->intervals()) {
                    if (other == changed) break;
                    if (other->type == RideFileInterval::USER && same(other->name, other->start, other->stop)) occurrence++;
                }
            }
            QString error;
            if (!env.session->rideCache()->saveActivity(item, error)) {
                fail(Status::Failed, error);
                return false;
            }
            if (!wasOpen) item->close();
            wasOpen = true; // closed, or the caller's to close
            env.session->refresh();
            return true;
        }

        // the interval given to save() as interval list now shows it, with
        // the sidebar's metrics
        QJsonObject saved()
        {
            int number = 0, seen = 0;
            for (IntervalItem *interval : item->intervals()) {
                number++;
                if (interval->type != RideFileInterval::USER || !same(interval->name, interval->start, interval->stop)) continue;
                if (seen++ < occurrence) continue;
                QJsonObject o = intervalJson(interval, number);
                o.insert("metrics", intervalMetrics(interval, summaryMetrics(item), false, true, false));
                return o;
            }
            return QJsonObject();
        }

        // the result: status updated, unchanged or dry run
        CommandResult result(QJsonObject data, bool changed, const QString &summary, const QJsonArray &rows)
        {
            QString status = !changed ? "unchanged" : dryRun ? "dry run" : "updated";
            data.insert("activity", id);
            data.insert("status", status);
            data.insert("saved", status == "updated");
            CommandResult r = CommandResult::success(data);
            r.text = QString("%1  %2  %3%4\n").arg(status, id, summary, changed && dryRun ? ", dry run, nothing saved" : "");
            if (!rows.isEmpty()) r.text += ResultFormat::table(rows, summaryMetrics(item));
            r.csv = ResultFormat::csv(QJsonObject{ { "intervals", rows } }, summaryMetrics(item));
            return r;
        }

        RideItem *item = nullptr;
        RideFile *ride = nullptr;
        QString id;
        bool dryRun = false;
        CommandResult failure;

    private:

        bool same(const QString &name, double start, double stop) const
        {
            return name == tracked.name && std::fabs(start - tracked.start) < 1e-6 && std::fabs(stop - tracked.stop) < 1e-6;
        }

        CommandEnvironment &env;
        bool wasOpen = true;
        struct { QString name; double start = 0, stop = 0; } tracked;
        int occurrence = 0;
};

// an interval as it will be, before it is saved
static QJsonObject
plannedJson(const QString &name, double start, double stop, const QColor &color, bool test)
{
    QJsonObject o;
    o.insert("name", name);
    o.insert("type", intervalTypeKey(RideFileInterval::USER));
    o.insert("start", jsonNumber(start));
    o.insert("stop", jsonNumber(stop));
    o.insert("duration", jsonNumber(stop - start));
    if (color.isValid() && color != Qt::black) o.insert("color", color.name());
    if (test) o.insert("test", true);
    return o;
}

static bool
colorArg(const CommandRequest &request, QColor &color, QString &error)
{
    if (!request.args.contains("color")) return true;
    QString text = request.args.value("color").toString().trimmed();
    color = QColor::fromString(text);
    if (!color.isValid()) {
        error = QString("--color '%1' isn't a colour, give a name (red) or #rrggbb").arg(text);
        return false;
    }
    return true;
}

static bool
timeArg(const CommandRequest &request, const QString &name, double &secs, QString &error)
{
    QString text = request.args.value(name).toString();
    if (parseTime(text, secs)) return true;
    error = QString("--%1 '%2' isn't a time from the start, give seconds (754) or minutes and seconds (12:34, 1:02:03)").arg(name, text);
    return false;
}

static CommandResult
addInterval(CommandEnvironment &env, const CommandRequest &request)
{
    const QJsonObject &args = request.args;
    bool from = args.contains("from");
    if (from && (args.contains("start") || args.contains("stop")))
        return CommandResult::failure(Status::Usage, "give --from or --start and --stop, not both");
    if (!from && !(args.contains("start") && args.contains("stop")))
        return CommandResult::failure(Status::Usage, "give --start and --stop, or --from an interval to copy");
    if (!from && !args.contains("name"))
        return CommandResult::failure(Status::Usage, "give the new interval a --name");

    QString error;
    QColor color = Qt::black; // the next of the standard colours
    if (!colorArg(request, color, error)) return CommandResult::failure(Status::Usage, error);
    double start = 0, stop = 0;
    if (!from && (!timeArg(request, "start", start, error) || !timeArg(request, "stop", stop, error)))
        return CommandResult::failure(Status::Usage, error);

    IntervalChange change(env, request);
    if (!change.ok()) return change.failure;

    // a copy of any interval, as the sidebar's "Create a performance test"
    QString name = args.value("name").toString().trimmed();
    if (from) {
        QList<int> numbers;
        QList<IntervalItem*> found;
        if (!change.find(args.value("from"), false, numbers, found)) return change.failure;
        if (found.count() > 1) return change.fail(Status::Usage, "--from takes one interval");
        start = found.first()->start;
        stop = found.first()->stop;
        if (name.isEmpty()) name = found.first()->name;
    }
    if (name.isEmpty()) return change.fail(Status::Usage, "the name can't be empty");
    bool test = args.value("test").toBool(false);
    if (!change.checkSpan(start, stop)) return change.failure;

    QJsonObject interval = plannedJson(name, start, stop, color, test);
    if (!change.dryRun) {
        IntervalItem *added = change.item->newInterval(name, start, stop, change.ride->timeToDistance(start),
                                                       change.ride->timeToDistance(stop), color, test);
        if (!change.save(added)) return change.failure;
        interval = change.saved();
    }
    QJsonObject data;
    data.insert("interval", interval);
    return change.result(data, true, QString("added '%1' %2-%3").arg(name, timeText(start), timeText(stop)), QJsonArray{ interval });
}

static CommandResult
editInterval(CommandEnvironment &env, const CommandRequest &request)
{
    const QJsonObject &args = request.args;
    if (!args.contains("name") && !args.contains("start") && !args.contains("stop") && !args.contains("color") && !args.contains("test"))
        return CommandResult::failure(Status::Usage, "nothing to change, give --name, --start, --stop, --color or --test");

    QString error;
    double start = 0, stop = 0;
    if (args.contains("start") && !timeArg(request, "start", start, error)) return CommandResult::failure(Status::Usage, error);
    if (args.contains("stop") && !timeArg(request, "stop", stop, error)) return CommandResult::failure(Status::Usage, error);
    QColor color;
    if (!colorArg(request, color, error)) return CommandResult::failure(Status::Usage, error);

    IntervalChange change(env, request);
    if (!change.ok()) return change.failure;
    QList<int> numbers;
    QList<IntervalItem*> found;
    if (!change.find(args.value("interval"), true, numbers, found)) return change.failure;
    if (found.count() > 1) return change.fail(Status::Usage, "give one interval to edit");
    IntervalItem *interval = found.first();

    // what isn't given stays as it is
    QString name = args.contains("name") ? args.value("name").toString().trimmed() : interval->name;
    if (name.isEmpty()) return change.fail(Status::Usage, "the name can't be empty");
    if (!args.contains("start")) start = interval->start;
    if (!args.contains("stop")) stop = interval->stop;
    // a lap without a colour of its own keeps getting one by its place
    if (!args.contains("color")) color = interval->rideInterval->color;
    bool test = args.contains("test") ? args.value("test").toBool() : interval->test;
    if (!change.checkSpan(start, stop)) return change.failure;

    QJsonObject before = intervalJson(interval, numbers.first());
    bool changed = name != interval->name || start != interval->start || stop != interval->stop
                   || color != interval->rideInterval->color || test != interval->test;
    QJsonObject after = changed ? plannedJson(name, start, stop, color, test) : before;
    if (changed && !change.dryRun) {
        interval->setValues(name, start, stop, change.ride->timeToDistance(start), change.ride->timeToDistance(stop), color, test);
        if (!change.save(interval)) return change.failure;
        after = change.saved();
    }
    QJsonObject data;
    data.insert("before", before);
    data.insert("interval", after);
    return change.result(data, changed, QString("interval %1 '%2' %3-%4").arg(numbers.first()).arg(name, timeText(start), timeText(stop)),
                         QJsonArray{ after });
}

static CommandResult
deleteIntervals(CommandEnvironment &env, const CommandRequest &request)
{
    IntervalChange change(env, request);
    if (!change.ok()) return change.failure;
    QList<int> numbers;
    QList<IntervalItem*> found;
    if (!change.find(request.args.value("interval"), true, numbers, found)) return change.failure;

    QJsonArray deleted;
    QStringList names;
    for (int k = 0; k < found.count(); k++) {
        deleted.append(intervalJson(found.at(k), numbers.at(k)));
        names << QString("%1 '%2'").arg(numbers.at(k)).arg(found.at(k)->name);
    }
    if (!change.dryRun) {
        for (IntervalItem *interval : found) {
            RideFileInterval *stored = interval->rideInterval;
            if (!change.item->removeInterval(interval))
                return change.fail(Status::Failed, QString("can't delete interval '%1'").arg(interval->name));
            delete stored;
            delete interval;
        }
        if (!change.save()) return change.failure;
    }
    QJsonObject data;
    data.insert("deleted", deleted);
    return change.result(data, true, QString("deleted %1").arg(names.join(", ")), deleted);
}

static CommandResult
renameIntervals(CommandEnvironment &env, const CommandRequest &request)
{
    QString name = request.args.value("name").toString().trimmed();
    if (name.isEmpty()) return CommandResult::failure(Status::Usage, "the name can't be empty");

    IntervalChange change(env, request);
    if (!change.ok()) return change.failure;
    QList<int> numbers;
    QList<IntervalItem*> found;
    if (!change.find(request.args.value("interval"), true, numbers, found)) return change.failure;

    // numbered in the order the intervals are listed, as the sidebar does
    QList<IntervalItem*> ordered;
    for (IntervalItem *interval : change.item->intervals(RideFileInterval::USER))
        if (found.contains(interval)) ordered << interval;
    QStringList before;
    for (IntervalItem *interval : ordered) before << interval->name;
    change.item->renameIntervals(ordered, name);

    QJsonArray renamed;
    bool changed = false;
    for (int k = 0; k < ordered.count(); k++) {
        QJsonObject o;
        o.insert("number", change.item->intervals().indexOf(ordered.at(k)) + 1);
        o.insert("from", before.at(k));
        o.insert("to", ordered.at(k)->name);
        renamed.append(o);
        if (before.at(k) != ordered.at(k)->name) changed = true;
    }
    if (changed && !change.save()) return change.failure;
    if (!changed) change.item->setDirty(false);

    QJsonObject data;
    data.insert("renamed", renamed);
    return change.result(data, changed, QString("renamed %1 intervals").arg(renamed.count()), renamed);
}

static CommandResult
sortIntervals(CommandEnvironment &env, const CommandRequest &request)
{
    IntervalChange change(env, request);
    if (!change.ok()) return change.failure;

    bool changed = change.item->sortIntervals();
    if (changed && !change.save()) return change.failure;

    QJsonArray order;
    int number = 0;
    for (IntervalItem *interval : change.item->intervals()) {
        number++;
        if (interval->type != RideFileInterval::USER) continue;
        order.append(intervalJson(interval, number));
    }
    QJsonObject data;
    data.insert("intervals", order);
    return change.result(data, changed, QString("%1 user intervals in time order").arg(order.count()), order);
}

static CommandResult
mergeIntervals(CommandEnvironment &env, const CommandRequest &request)
{
    IntervalChange change(env, request);
    if (!change.ok()) return change.failure;
    QList<int> numbers;
    QList<IntervalItem*> found;
    if (!change.find(request.args.value("interval"), true, numbers, found)) return change.failure;
    if (found.count() < 2) return change.fail(Status::Usage, "give two or more intervals to merge");

    // the first to start becomes the merged interval, the others go
    int first = 0;
    double start = found.first()->start, stop = found.first()->stop;
    for (int k = 1; k < found.count(); k++) {
        if (found.at(k)->start < found.at(first)->start) first = k;
        start = std::min(start, found.at(k)->start);
        stop = std::max(stop, found.at(k)->stop);
    }
    IntervalItem *kept = found.at(first);
    QString name = request.args.contains("name") ? request.args.value("name").toString().trimmed() : kept->name;
    if (name.isEmpty()) return change.fail(Status::Usage, "the name can't be empty");

    QJsonArray merged;
    QStringList listed;
    for (int k = 0; k < found.count(); k++) {
        merged.append(intervalJson(found.at(k), numbers.at(k)));
        listed << QString::number(numbers.at(k));
    }

    // laps inside the span that weren't given stay as they are
    QStringList inside;
    int number = 0;
    for (IntervalItem *interval : change.item->intervals()) {
        number++;
        if (interval->type != RideFileInterval::USER || found.contains(interval)) continue;
        if (interval->start < stop && interval->stop > start) inside << QString("%1 '%2'").arg(number).arg(interval->name);
    }

    if (!change.checkSpan(start, stop, "; to drop the laps, use 'interval delete'")) return change.failure;

    // a lap without a colour of its own keeps getting one by its place
    QColor color = kept->rideInterval->color;
    QJsonObject interval = plannedJson(name, start, stop, color, kept->test);
    if (!change.dryRun) {
        kept->setValues(name, start, stop, change.ride->timeToDistance(start), change.ride->timeToDistance(stop), color, kept->test);
        for (IntervalItem *other : found) {
            if (other == kept) continue;
            RideFileInterval *stored = other->rideInterval;
            if (!change.item->removeInterval(other))
                return change.fail(Status::Failed, QString("can't delete interval '%1'").arg(other->name));
            delete stored;
            delete other;
        }
        if (!change.save(kept)) return change.failure;
        interval = change.saved();
    }

    QJsonObject data;
    data.insert("merged", merged);
    data.insert("interval", interval);
    CommandResult r = change.result(data, true, QString("merged %1 into '%2' %3-%4").arg(listed.join(", "), name, timeText(start), timeText(stop)),
                                    QJsonArray{ interval });
    if (!inside.isEmpty())
        r.warnings << QString("not merged, though inside the merged interval: %1").arg(inside.join(", "));
    return r;
}

static CommandResult
showInterval(CommandEnvironment &env, const CommandRequest &request)
{
    QString error;
    RideItem *item = env.session->findActivity(request.args.value("activity").toString(), error);
    if (!item) return CommandResult::failure(Status::NotFound, error);

    bool metricUnits = !request.args.value("imperial").toBool(false);
    bool display = request.args.value("display").toBool(false);
    QStringList symbols;
    if (!resolveMetrics(splitList(request.args.value("metric")), symbols, error)) return CommandResult::failure(Status::Usage, error);

    // by number as interval list shows it, or by name
    QList<int> numbers;
    Status status;
    if (!findIntervals(item, QStringList{ request.args.value("interval").toString() }, numbers, status, error))
        return CommandResult::failure(status, error);

    if (numbers.count() > 1)
        return CommandResult::failure(Status::Usage, "give one interval, 'interval list' shows them all");

    IntervalItem *interval = item->intervals().at(numbers.first() - 1);
    QJsonObject o = intervalJson(interval, numbers.first());
    o.insert("activity", QFileInfo(item->fileName).completeBaseName());
    o.insert("metrics", intervalMetrics(interval, symbols, symbols.isEmpty(), metricUnits, display));
    return CommandResult::success(o);
}

void
registerIntervalCommands(CommandRegistry &registry)
{
    Command list;
    list.spec.name = "interval.list";
    list.spec.summary = "list an activity's intervals (laps, efforts, climbs ...) with metrics";
    list.spec.description = "Metrics default to the ones the GUI's intervals sidebar shows. For the interval "
                            "tables on the activity overview, as the GUI draws them, see 'activity overview'. "
                            "The result counts user intervals (laps, from the device or marked in GoldenCheetah) "
                            "and discovered efforts, including zeros. Those counts are for the whole activity, "
                            "even when --type filters the rows.";
    list.spec.scope = Scope::Athlete;
    list.spec.params << ParamSpec("activity", ParamType::String, "activity id, start time, date, 'first' or 'last'").req().pos();
    // checked by the handler, so a comma separated list works too
    list.spec.params << ParamSpec("type", ParamType::String,
                                  QString("only these interval types: %1 (or the sidebar's group titles)").arg(intervalTypeNames().join(", "))).many();
    list.spec.params << ParamSpec("metric", ParamType::String, "metric symbols or formula names, e.g. Average_Power (comma separated or repeated)").many();
    list.spec.params << ParamSpec("imperial", ParamType::Bool, "metric values in imperial units");
    list.spec.params << ParamSpec("display", ParamType::Bool, "metric values as the GUI shows them, e.g. 51:51 or 160");
    list.spec.httpMethod = "GET";
    list.spec.httpPath = "/athletes/{athlete}/activities/{activity}/intervals";
    list.handler = listIntervals;
    registry.add(list);

    Command show;
    show.spec.name = "interval.show";
    show.spec.summary = "show one interval of an activity with all its metrics";
    show.spec.scope = Scope::Athlete;
    show.spec.params << ParamSpec("activity", ParamType::String, "activity id, start time, date, 'first' or 'last'").req().pos();
    show.spec.params << ParamSpec("interval", ParamType::String, "interval number (see 'interval list') or name").req().pos();
    show.spec.params << ParamSpec("metric", ParamType::String, "only these metrics, by symbol or formula name (default: every one relevant for the activity)").many();
    show.spec.params << ParamSpec("imperial", ParamType::Bool, "metric values in imperial units");
    show.spec.params << ParamSpec("display", ParamType::Bool, "metric values as the GUI shows them, e.g. 51:51 or 160");
    show.spec.httpMethod = "GET";
    show.spec.httpPath = "/athletes/{athlete}/activities/{activity}/intervals/{interval}";
    show.handler = showInterval;
    registry.add(show);

    // the laps and marked intervals, changed as in the intervals sidebar
    const QString activityHelp = "activity id, start time, date, 'first' or 'last'";
    const QString intervalsHelp = "interval numbers as 'interval list' shows them, ranges (2-4) or names, comma separated or repeated";
    const QString timeHelp = "seconds from the start (754) or minutes and seconds (12:34, 1:02:03)";
    const QString saved = "Only laps and marked intervals (type user) can be changed: they are saved in the "
                          "activity file. The others are found again on every refresh. The activity is saved "
                          "as the GUI saves it and its metrics are recomputed.";

    Command add;
    add.spec.name = "interval.add";
    add.spec.summary = "add a lap or marked interval to an activity";
    add.spec.description = "The span is --start and --stop, or --from an interval of any type (an effort, a "
                           "climb ...), as the sidebar's \"Create a performance test\" copies one.\n" + saved;
    add.spec.scope = Scope::Athlete;
    add.spec.modifies = true;
    add.spec.params << ParamSpec("activity", ParamType::String, activityHelp).req().pos();
    add.spec.params << ParamSpec("name", ParamType::String, "name of the new interval (default with --from: that interval's name)");
    add.spec.params << ParamSpec("start", ParamType::String, "when it starts, " + timeHelp);
    add.spec.params << ParamSpec("stop", ParamType::String, "when it ends, " + timeHelp);
    add.spec.params << ParamSpec("from", ParamType::String, "copy the span of this interval, by number or name");
    add.spec.params << ParamSpec("color", ParamType::String, "colour, a name (red) or #rrggbb (default: the next standard colour)");
    add.spec.params << ParamSpec("test", ParamType::Bool, "mark it as a performance test");
    add.spec.params << ParamSpec("dry-run", ParamType::Bool, "report what would be added, save nothing");
    add.spec.httpMethod = "POST";
    add.spec.httpPath = "/athletes/{athlete}/activities/{activity}/intervals";
    add.handler = addInterval;
    registry.add(add);

    Command edit;
    edit.spec.name = "interval.edit";
    edit.spec.summary = "change a lap or marked interval: name, start, stop, colour, performance test";
    edit.spec.description = "As the sidebar's Edit interval dialog. What isn't given stays as it is.\n" + saved;
    edit.spec.scope = Scope::Athlete;
    edit.spec.modifies = true;
    edit.spec.params << ParamSpec("activity", ParamType::String, activityHelp).req().pos();
    edit.spec.params << ParamSpec("interval", ParamType::String, "interval number (see 'interval list') or name").req().pos();
    edit.spec.params << ParamSpec("name", ParamType::String, "new name");
    edit.spec.params << ParamSpec("start", ParamType::String, "new start, " + timeHelp);
    edit.spec.params << ParamSpec("stop", ParamType::String, "new end, " + timeHelp);
    edit.spec.params << ParamSpec("color", ParamType::String, "new colour, a name (red) or #rrggbb");
    edit.spec.params << ParamSpec("test", ParamType::Bool, "mark as a performance test, --no-test to unmark");
    edit.spec.params << ParamSpec("dry-run", ParamType::Bool, "report what would change, save nothing");
    edit.spec.httpMethod = "PATCH";
    edit.spec.httpPath = "/athletes/{athlete}/activities/{activity}/intervals/{interval}";
    edit.handler = editInterval;
    registry.add(edit);

    Command remove;
    remove.spec.name = "interval.delete";
    remove.spec.summary = "delete laps or marked intervals from an activity";
    remove.spec.description = saved;
    remove.spec.scope = Scope::Athlete;
    remove.spec.modifies = true;
    remove.spec.params << ParamSpec("activity", ParamType::String, activityHelp).req().pos();
    remove.spec.params << ParamSpec("interval", ParamType::String, intervalsHelp).req().pos().many();
    remove.spec.params << ParamSpec("dry-run", ParamType::Bool, "report what would be deleted, save nothing");
    remove.spec.httpMethod = "DELETE";
    remove.spec.httpPath = "/athletes/{athlete}/activities/{activity}/intervals/{interval}";
    remove.handler = deleteIntervals;
    registry.add(remove);

    Command rename;
    rename.spec.name = "interval.rename";
    rename.spec.summary = "rename laps or marked intervals with a numbered name";
    rename.spec.description = "As the sidebar's \"Rename selected intervals\": the intervals are numbered in the "
                              "order 'interval list' shows them, from the number the name ends in (\"Rep 3\" gives "
                              "Rep 3, Rep 4 ...) or from 1 (\"Rep\" gives Rep 1, Rep 2 ...). For one name, "
                              "use 'interval edit --name'.\n" + saved;
    rename.spec.scope = Scope::Athlete;
    rename.spec.modifies = true;
    rename.spec.params << ParamSpec("activity", ParamType::String, activityHelp).req().pos();
    rename.spec.params << ParamSpec("interval", ParamType::String, intervalsHelp).req().pos().many();
    rename.spec.params << ParamSpec("name", ParamType::String, "the name, numbered from the number it ends in, else from 1").req();
    rename.spec.httpMethod = "POST";
    rename.spec.httpPath = "/athletes/{athlete}/activities/{activity}/intervals/rename";
    rename.handler = renameIntervals;
    registry.add(rename);

    Command sort;
    sort.spec.name = "interval.sort";
    sort.spec.summary = "put an activity's laps and marked intervals in time order";
    sort.spec.description = "As the sidebar's \"Sort User Intervals\".\n" + saved;
    sort.spec.scope = Scope::Athlete;
    sort.spec.modifies = true;
    sort.spec.params << ParamSpec("activity", ParamType::String, activityHelp).req().pos();
    sort.spec.httpMethod = "POST";
    sort.spec.httpPath = "/athletes/{athlete}/activities/{activity}/intervals/sort";
    sort.handler = sortIntervals;
    registry.add(sort);

    Command merge;
    merge.spec.name = "interval.merge";
    merge.spec.summary = "merge laps or marked intervals into one";
    merge.spec.description = "The merged interval runs from the earliest start to the latest end of the intervals "
                             "given. It is the first of them to start, extended, keeping its name (unless --name), "
                             "colour and performance test mark. The others are deleted. Laps inside that span that "
                             "weren't given stay as they are, with a warning.\n" + saved;
    merge.spec.scope = Scope::Athlete;
    merge.spec.modifies = true;
    merge.spec.params << ParamSpec("activity", ParamType::String, activityHelp).req().pos();
    merge.spec.params << ParamSpec("interval", ParamType::String, intervalsHelp + ", two or more").req().pos().many();
    merge.spec.params << ParamSpec("name", ParamType::String, "name of the merged interval (default: the first one's)");
    merge.spec.params << ParamSpec("dry-run", ParamType::Bool, "report what would be merged, save nothing");
    merge.spec.httpMethod = "POST";
    merge.spec.httpPath = "/athletes/{athlete}/activities/{activity}/intervals/merge";
    merge.handler = mergeIntervals;
    registry.add(merge);
}

} // namespace Headless
