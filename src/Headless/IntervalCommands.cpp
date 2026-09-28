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
    o.insert("type", RideFileInterval::typeDescription(interval->type));
    o.insert("start", jsonNumber(interval->start));
    o.insert("stop", jsonNumber(interval->stop));
    o.insert("duration", jsonNumber(interval->stop - interval->start));
    o.insert("start_km", jsonNumber(interval->startKM));
    o.insert("stop_km", jsonNumber(interval->stopKM));
    o.insert("color", interval->color.name());
    if (interval->test) o.insert("test", true);
    return o;
}

// metric values, all non-zero ones when no symbols are given
static QJsonObject
intervalMetrics(IntervalItem *interval, const QStringList &symbols, bool metricUnits)
{
    const RideMetricFactory &factory = RideMetricFactory::instance();
    QJsonObject m;
    // metrics are computed when the ride cache refreshes, none means not yet
    if (interval->metrics().size() != factory.metricCount()) return m;

    if (symbols.isEmpty()) {
        for (int i = 0; i < factory.metricCount(); i++) {
            QString symbol = factory.metricName(i);
            double v = interval->getForSymbol(symbol, metricUnits);
            if (std::isnan(v) || std::isinf(v) || v == 0) continue;
            m.insert(symbol, jsonNumber(v));
        }
    } else {
        for (const QString &symbol : symbols) m.insert(symbol, jsonNumber(interval->getForSymbol(symbol, metricUnits)));
    }
    return m;
}

static bool
checkMetrics(const QStringList &symbols, QString &error)
{
    for (const QString &s : symbols) {
        if (!RideMetricFactory::instance().haveMetric(s)) {
            error = QString("unknown metric '%1', see 'metric list'").arg(s);
            return false;
        }
    }
    return true;
}

static CommandResult
listIntervals(CommandEnvironment &env, const CommandRequest &request)
{
    QString error;
    RideItem *item = env.session->findActivity(request.args.value("activity").toString(), error);
    if (!item) return CommandResult::failure(Status::NotFound, error);

    bool metricUnits = !request.args.value("imperial").toBool(false);
    QStringList symbols = splitList(request.args.value("metric"));
    if (!checkMetrics(symbols, error)) return CommandResult::failure(Status::Usage, error);
    if (symbols.isEmpty()) symbols = summaryMetrics(item);

    QList<RideFileInterval::IntervalType> types;
    for (const QString &t : splitList(request.args.value("type"))) {
        bool found = false;
        for (const auto &known : intervalTypes()) if (known.first == t.toLower()) { types << known.second; found = true; }
        if (!found) return CommandResult::failure(Status::Usage,
                        QString("unknown interval type '%1', choose from: %2").arg(t).arg(intervalTypeNames().join(", ")));
    }

    QJsonArray list;
    int number = 0;
    for (IntervalItem *interval : item->intervals()) {
        number++;
        if (!types.isEmpty() && !types.contains(interval->type)) continue;
        QJsonObject o = intervalJson(interval, number);
        o.insert("metrics", intervalMetrics(interval, symbols, metricUnits));
        list.append(o);
    }
    QJsonObject data;
    data.insert("activity", QFileInfo(item->fileName).completeBaseName());
    data.insert("intervals", list);
    return CommandResult::success(data);
}

static CommandResult
showInterval(CommandEnvironment &env, const CommandRequest &request)
{
    QString error;
    RideItem *item = env.session->findActivity(request.args.value("activity").toString(), error);
    if (!item) return CommandResult::failure(Status::NotFound, error);

    bool metricUnits = !request.args.value("imperial").toBool(false);
    QStringList symbols = splitList(request.args.value("metric"));
    if (!checkMetrics(symbols, error)) return CommandResult::failure(Status::Usage, error);

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
    o.insert("metrics", intervalMetrics(interval, symbols, metricUnits));
    return CommandResult::success(o);
}

void
registerIntervalCommands(CommandRegistry &registry)
{
    Command list;
    list.spec.name = "interval.list";
    list.spec.summary = "list an activity's intervals (laps, efforts, climbs ...) with metrics";
    list.spec.description = "Metrics default to the ones the GUI's intervals sidebar shows.";
    list.spec.scope = Scope::Athlete;
    list.spec.params << ParamSpec("activity", ParamType::String, "activity id, start time, date, 'first' or 'last'").req().pos();
    // checked by the handler, so a comma separated list works too
    list.spec.params << ParamSpec("type", ParamType::String,
                                  QString("only these interval types: %1").arg(intervalTypeNames().join(", "))).many();
    list.spec.params << ParamSpec("metric", ParamType::String, "metric symbols to include (comma separated or repeated)").many();
    list.spec.params << ParamSpec("imperial", ParamType::Bool, "metric values in imperial units");
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
    show.spec.params << ParamSpec("metric", ParamType::String, "only these metric symbols (default: every one with a value)").many();
    show.spec.params << ParamSpec("imperial", ParamType::Bool, "metric values in imperial units");
    show.spec.httpMethod = "GET";
    show.spec.httpPath = "/athletes/{athlete}/activities/{activity}/intervals/{interval}";
    show.handler = showInterval;
    registry.add(show);
}

} // namespace Headless
