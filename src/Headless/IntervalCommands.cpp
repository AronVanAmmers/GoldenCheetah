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
// intervals sidebar and the interval tables show them.
//

#include "HeadlessCommands.h"
#include "ActivitySelection.h"
#include "ResultFormat.h"

#include "Settings.h"
#include "RideItem.h"
#include "RideFile.h"
#include "IntervalItem.h"
#include "RideMetric.h"

#include <QFileInfo>
#include <cmath>

namespace Headless {

// stable names for the interval types, the GUI's group titles are translated
static const QList<QPair<QString, RideFileInterval::IntervalType>> &
intervalTypes()
{
    static const QList<QPair<QString, RideFileInterval::IntervalType>> types = {
        { "user", RideFileInterval::USER }, { "all", RideFileInterval::ALL },
        { "device", RideFileInterval::DEVICE }, { "peakpower", RideFileInterval::PEAKPOWER },
        { "peakpace", RideFileInterval::PEAKPACE }, { "effort", RideFileInterval::EFFORT },
        { "route", RideFileInterval::ROUTE }, { "climb", RideFileInterval::CLIMB }
    };
    return types;
}

QString
intervalTypeKey(int type)
{
    for (const auto &t : intervalTypes()) if (t.second == type) return t.first;
    return QString();
}

IntervalCensus
intervalCensus(RideItem *item)
{
    IntervalCensus census;
    if (!item) return census;
    for (IntervalItem *interval : item->intervals()) {
        switch (interval->type) {
        case RideFileInterval::DEVICE: census.recordedLaps++; break;
        case RideFileInterval::USER: census.userIntervals++; break;
        case RideFileInterval::EFFORT:
        case RideFileInterval::PEAKPOWER:
        case RideFileInterval::PEAKPACE:
        case RideFileInterval::CLIMB:
        case RideFileInterval::ROUTE: census.discoveredEfforts++; break;
        default: break; // the entire activity
        }
    }
    return census;
}

QString
intervalCensusLine(const IntervalCensus &census)
{
    return QString("recorded laps: %1, user intervals: %2, discovered efforts: %3")
        .arg(census.recordedLaps).arg(census.userIntervals).arg(census.discoveredEfforts);
}

// a type by its key or by the group title the GUI shows (EFFORTS, PEAK POWER)
static bool
intervalTypeFromName(const QString &name, RideFileInterval::IntervalType &type)
{
    QString n = QString(name).remove(' ').toLower();
    for (const auto &t : intervalTypes()) {
        QString title = RideFileInterval::typeDescription(t.second).remove(' ').toLower();
        if (n == t.first || n == title || n + "s" == title) { type = t.second; return true; }
    }
    return false;
}

static QStringList
intervalTypeNames()
{
    QStringList names;
    for (const auto &t : intervalTypes()) names << t.first;
    return names;
}

// the metrics the intervals sidebar shows, as set in the GUI's preferences
static QStringList
summaryMetrics(RideItem *item)
{
    QString s = appsettings->contains(GC_SETTINGS_FAVOURITE_METRICS)
                ? appsettings->value(nullptr, GC_SETTINGS_FAVOURITE_METRICS).toString()
                : QString(GC_SETTINGS_FAVOURITE_METRICS_DEFAULT);
    QStringList symbols;
    const RideMetricFactory &factory = RideMetricFactory::instance();
    for (const QString &symbol : s.split(",", Qt::SkipEmptyParts)) {
        const RideMetric *m = factory.rideMetric(symbol.trimmed());
        if (m && m->isRelevantForRide(item)) symbols << m->symbol();
    }
    return symbols;
}

static QJsonObject
intervalJson(IntervalItem *interval, int number)
{
    QJsonObject o;
    o.insert("number", number);
    o.insert("name", interval->name);
    o.insert("type", intervalTypeKey(interval->type));
    o.insert("group", RideFileInterval::typeDescription(interval->type));   // as the sidebar titles it
    o.insert("start", jsonNumber(interval->start));
    o.insert("stop", jsonNumber(interval->stop));
    o.insert("duration", jsonNumber(interval->stop - interval->start));
    o.insert("start_km", jsonNumber(interval->startKM));
    o.insert("stop_km", jsonNumber(interval->stopKM));
    o.insert("color", interval->color.name());
    if (interval->test) o.insert("test", true);
    return o;
}

// metric values, every one relevant for the activity when no symbols are
// given; as numbers, or with display as the GUI formats them ("51:51")
static QJsonObject
intervalMetrics(IntervalItem *interval, QStringList symbols, bool metricUnits, bool display)
{
    const RideMetricFactory &factory = RideMetricFactory::instance();
    QJsonObject m;
    // metrics are computed when the ride cache refreshes, none means not yet
    if (interval->metrics().size() != factory.metricCount()) return m;

    if (symbols.isEmpty()) {
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
    if (symbols.isEmpty()) symbols = summaryMetrics(item);

    QList<RideFileInterval::IntervalType> types;
    for (const QString &t : splitList(request.args.value("type"))) {
        RideFileInterval::IntervalType type;
        if (!intervalTypeFromName(t, type))
            return CommandResult::failure(Status::Usage,
                        QString("unknown interval type '%1', choose from: %2").arg(t).arg(intervalTypeNames().join(", ")));
        types << type;
    }

    QJsonArray list;
    int number = 0;
    for (IntervalItem *interval : item->intervals()) {
        number++;
        if (!types.isEmpty() && !types.contains(interval->type)) continue;
        QJsonObject o = intervalJson(interval, number);
        o.insert("metrics", intervalMetrics(interval, symbols, metricUnits, display));
        list.append(o);
    }
    IntervalCensus census = intervalCensus(item);
    QJsonObject data;
    data.insert("activity", QFileInfo(item->fileName).completeBaseName());
    data.insert("recorded_laps", census.recordedLaps);
    data.insert("user_intervals", census.userIntervals);
    data.insert("discovered_efforts", census.discoveredEfforts);
    data.insert("intervals", list);

    // the counts are for the whole activity, so they stay when --type
    // filters the table. Text and CSV lead with that one line.
    QJsonObject table = data;
    table.remove("recorded_laps");
    table.remove("user_intervals");
    table.remove("discovered_efforts");
    CommandResult result = CommandResult::success(data);
    QString line = intervalCensusLine(census);
    // metric columns follow the favourites, or --metric, rather than sorted keys
    result.text = line + "\n" + ResultFormat::render(table, symbols);
    result.csv = ResultFormat::csvLine({ line }) + ResultFormat::csv(table, symbols);
    return result;
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
    QString id = request.args.value("interval").toString().trimmed();
    const QList<IntervalItem*> &intervals = item->intervals();
    bool isNumber = false;
    int n = id.toInt(&isNumber);
    QList<int> matches;
    if (isNumber) {
        if (n >= 1 && n <= intervals.count()) matches << n;
    } else {
        for (int i = 0; i < intervals.count(); i++)
            if (intervals[i]->name.compare(id, Qt::CaseInsensitive) == 0) matches << i + 1;
    }
    if (matches.isEmpty())
        return CommandResult::failure(Status::NotFound,
                    QString("no interval '%1' in %2, see 'interval list'").arg(id).arg(QFileInfo(item->fileName).completeBaseName()));
    if (matches.count() > 1) {
        QStringList numbers;
        for (int m : matches) numbers << QString::number(m);
        return CommandResult::failure(Status::Usage,
                    QString("more than one interval is called '%1', choose one by number (%2)").arg(id).arg(numbers.join(", ")));
    }

    IntervalItem *interval = intervals[matches.first() - 1];
    QJsonObject o = intervalJson(interval, matches.first());
    o.insert("activity", QFileInfo(item->fileName).completeBaseName());
    o.insert("metrics", intervalMetrics(interval, symbols, metricUnits, display));
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
                            "The result counts recorded laps, user intervals and discovered efforts, including "
                            "zeros. Those counts are for the whole activity, even when --type filters the rows.";
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
}

} // namespace Headless
