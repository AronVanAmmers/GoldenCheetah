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
#include "MetricNames.h"
#include "ActivityJson.h"
#include "IntervalData.h"
#include "ResultFormat.h"

#include "Settings.h"
#include "RideItem.h"
#include "RideFile.h"
#include "IntervalItem.h"
#include "RideMetric.h"

#include <QFileInfo>
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
        o.insert("metrics", intervalMetrics(interval, symbols, false, metricUnits, display));
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
    // filters the table. The text report leads with that one line.
    // CSV is the table, starting at the header.
    QJsonObject table = data;
    table.remove("recorded_laps");
    table.remove("user_intervals");
    table.remove("discovered_efforts");
    CommandResult result = CommandResult::success(data);
    QString line = intervalCensusLine(census);
    // metric columns follow the favourites, or --metric, rather than sorted keys
    result.text = line + "\n" + ResultFormat::render(table, symbols);
    result.csv = ResultFormat::csv(table, symbols);
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
