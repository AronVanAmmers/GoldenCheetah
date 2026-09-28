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
// Charts as images: the headless counterparts of the activity plot, the
// critical power chart, the performance manager, time in zones and trends.
//

#include "HeadlessCommands.h"
#include "ChartRenderer.h"
#include "MetricData.h"
#include "ZoneData.h"
#include "ActivitySelection.h"

#include "Context.h"
#include "Athlete.h"
#include "RideCache.h"
#include "RideItem.h"
#include "RideFile.h"
#include "RideMetric.h"
#include "PMCData.h"
#include "PDModel.h"
#include "Zones.h"
#include "HrZones.h"
#include "Colors.h"
#include "WPrime.h"

#include <QFileInfo>
#include <cmath>

namespace Headless {

static QList<ParamSpec>
imageParams()
{
    QList<ParamSpec> list;
    list << ParamSpec("as", ParamType::String, "image format").def("png").oneOf(ChartRenderer::formats());
    list << ParamSpec("width", ParamType::Int, "width in pixels").def(1200);
    list << ParamSpec("height", ParamType::Int, "height in pixels").def(600);
    list << ParamSpec("dark", ParamType::Bool, "dark background");
    list << ParamSpec("title", ParamType::String, "chart title");
    return list;
}

static CommandResult
renderResult(ChartSpec spec, const CommandRequest &request, const QString &name, QJsonObject data)
{
    spec.size = QSize(request.args.value("width").toInt(1200), request.args.value("height").toInt(600));
    spec.dark = request.args.value("dark").toBool(false);
    if (request.args.contains("title")) spec.title = request.args.value("title").toString();

    QString format = request.args.value("as").toString("png");
    QString error;
    QByteArray bytes = ChartRenderer::render(spec, format, error);
    if (bytes.isEmpty()) return CommandResult::failure(Status::Failed, error.isEmpty() ? QString("could not draw the chart") : error);

    CommandResult result;
    result.payload = bytes;
    result.payloadType = ChartRenderer::mimeType(format);
    result.payloadName = name + "." + format;
    data.insert("format", format);
    data.insert("width", spec.size.width());
    data.insert("height", spec.size.height());
    data.insert("bytes", bytes.size());
    result.data = data;
    return result;
}

struct SeriesInfo {
    QString name;
    RideFile::SeriesType type;
    int color;
    QString label;
};

static const QList<SeriesInfo> &
plotSeries()
{
    static const QList<SeriesInfo> list = {
        { "watts", RideFile::watts, CPOWER, "Power (W)" },
        { "hr", RideFile::hr, CHEARTRATE, "Heart rate (bpm)" },
        { "cad", RideFile::cad, CCADENCE, "Cadence (rpm)" },
        { "speed", RideFile::kph, CSPEED, "Speed (km/h)" },
        { "alt", RideFile::alt, CALTITUDE, "Altitude (m)" },
        { "nm", RideFile::nm, CTORQUE, "Torque (Nm)" },
        { "wbal", RideFile::wbal, CWBAL, "W' balance (kJ)" },
    };
    return list;
}

static QStringList
plotSeriesNames()
{
    QStringList names;
    for (const SeriesInfo &s : plotSeries()) names << s.name;
    return names;
}

// moving average to calm the plot down, as the GUI's smoothing
static QVector<double>
smooth(const QVector<double> &y, int window)
{
    if (window <= 1) return y;
    QVector<double> out(y.count());
    double sum = 0;
    for (int i = 0; i < y.count(); i++) {
        sum += y[i];
        if (i >= window) sum -= y[i - window];
        out[i] = sum / std::min(i + 1, window);
    }
    return out;
}

static CommandResult
activityChart(CommandEnvironment &env, const CommandRequest &request)
{
    QString error;
    RideItem *item = env.session->findActivity(request.args.value("activity").toString(), error);
    if (!item) return CommandResult::failure(Status::NotFound, error);
    RideFile *ride = item->ride();
    if (!ride) return CommandResult::failure(Status::Failed, "can't open the activity file");

    QStringList wanted;
    for (const QString &s : splitList(request.args.value("series"))) wanted << s.toLower();
    bool automatic = wanted.isEmpty();
    int window = request.args.value("smooth").toInt(1);
    bool byDistance = request.args.value("distance").toBool(false);

    ChartSpec spec;
    spec.title = QString("%1  %2").arg(item->sport).arg(activityStart(item).replace("T", " "));
    spec.xLabel = byDistance ? "Distance (km)" : "Time";

    for (const SeriesInfo &info : plotSeries()) {
        if (!automatic && !wanted.contains(info.name)) continue;
        if (info.type == RideFile::wbal) {
            if (!ride->isDataPresent(RideFile::watts)) continue;
        } else if (!ride->isDataPresent(info.type)) continue;
        if (automatic && (info.type == RideFile::nm || info.type == RideFile::wbal)) continue;

        ChartSeries s;
        s.name = info.label;
        s.color = GColor(info.color);
        s.style = info.type == RideFile::alt ? ChartSeries::Area : ChartSeries::Line;
        s.width = 1.2;
        QVector<double> y;
        if (info.type == RideFile::wbal) {
            WPrime *wp = ride->wprimeData();
            if (!wp) continue;
            s.x = wp->xdata(byDistance);
            for (double v : wp->ydata()) y << v / 1000.0;
        } else {
            for (RideFilePoint *p : ride->dataPoints()) {
                s.x << (byDistance ? p->km : p->secs);
                y << p->value(info.type);
            }
        }
        s.y = (info.type == RideFile::alt || info.type == RideFile::wbal) ? y : smooth(y, window);
        if (s.x.count() > s.y.count()) s.x.resize(s.y.count());

        ChartPanel panel;
        panel.yLabel = info.label;
        panel.xAxis = byDistance ? ChartPanel::Plain : ChartPanel::Duration;
        panel.series << s;
        panel.legend = false;
        spec.panels << panel;
    }
    if (spec.panels.isEmpty()) return CommandResult::failure(Status::Failed, "the activity has none of the requested data series");

    QJsonObject data;
    data.insert("activity", QFileInfo(item->fileName).completeBaseName());
    return renderResult(spec, request, QFileInfo(item->fileName).completeBaseName(), data);
}

static CommandResult
meanMaxChart(CommandEnvironment &env, const CommandRequest &request)
{
    QString error;
    Status status;
    int count = 0;
    QVector<double> data = meanMax(*env.session, powerSelection(request.args), RideFile::watts, error, status, count);
    if (!error.isEmpty()) return CommandResult::failure(status, error);
    if (data.count() < 2) return CommandResult::failure(Status::Failed, "no power data in the chosen activities");

    ChartSeries bests;
    bests.name = "Best power";
    bests.color = GColor(CPOWER);
    bests.width = 2;
    for (int i = 1; i < data.count(); i++) {
        if (data[i] <= 0) continue;
        bests.x << i;
        bests.y << data[i];
    }

    ChartPanel panel;
    panel.xAxis = ChartPanel::LogDuration;
    panel.yLabel = "Power (W)";
    panel.series << bests;

    QJsonObject out;
    out.insert("activities", count);

    QString modelName = request.args.value("model").toString();
    if (modelName != "none") {
        std::unique_ptr<PDModel> model(fitModel(env.session->context(), modelName, data));
        if (model) {
            ChartSeries fit;
            fit.name = model->name();
            fit.color = GColor(CCP);
            fit.dashed = true;
            for (double t : bests.x) {
                if (t < 1) continue;
                fit.x << t;
                fit.y << model->y(t / 60.0); // the model works in minutes, see fitModel
            }
            panel.series << fit;
            if (model->hasCP()) out.insert("cp", std::round(model->CP()));
            if (model->hasWPrime()) out.insert("wprime", std::round(model->WPrime()));
            if (model->hasPMax()) out.insert("pmax", std::round(model->PMax()));
            QString summary;
            if (model->hasCP()) summary += QString("CP %1 W").arg(std::round(model->CP()));
            if (model->hasWPrime()) summary += QString("   W' %1 kJ").arg(model->WPrime() / 1000.0, 0, 'f', 1);
            if (model->hasPMax()) summary += QString("   Pmax %1 W").arg(std::round(model->PMax()));
            panel.title = summary;
        }
    }

    ChartSpec spec;
    spec.title = "Power-duration";
    spec.xLabel = "Duration";
    spec.panels << panel;
    return renderResult(spec, request, "meanmax", out);
}

static CommandResult
pmcChart(CommandEnvironment &env, const CommandRequest &request)
{
    QString metric = request.args.value("metric").toString();
    if (!RideMetricFactory::instance().haveMetric(metric))
        return CommandResult::failure(Status::Usage, QString("unknown metric '%1'").arg(metric));

    PMCData *pmc = pmcFor(*env.session, metric, -1, -1);
    if (!pmc || !pmc->start().isValid()) return CommandResult::failure(Status::Failed, "no activities to compute the PMC from");

    QDate to = request.args.contains("to") ? QDate::fromString(request.args.value("to").toString(), Qt::ISODate) : QDate::currentDate();
    QDate from = request.args.contains("from") ? QDate::fromString(request.args.value("from").toString(), Qt::ISODate) : to.addDays(-180);
    if (from < pmc->start()) from = pmc->start();
    if (to > pmc->end()) to = pmc->end();

    ChartSeries ctl, atl, tsb, stress;
    ctl.name = "CTL"; ctl.color = GColor(CLTS); ctl.width = 2;
    atl.name = "ATL"; atl.color = GColor(CSTS); atl.width = 1.5;
    tsb.name = "TSB"; tsb.color = GColor(CSB); tsb.width = 1.5; tsb.rightAxis = true;
    stress.name = "Stress"; stress.color = GColor(CPLOTMARKER); stress.style = ChartSeries::Dots;

    QDate epoch(1970, 1, 1);
    for (QDate d = from; d <= to; d = d.addDays(1)) {
        double x = epoch.daysTo(d);
        ctl.x << x; ctl.y << pmc->lts(d);
        atl.x << x; atl.y << pmc->sts(d);
        tsb.x << x; tsb.y << pmc->sb(d);
        if (pmc->stress(d) > 0) { stress.x << x; stress.y << pmc->stress(d); }
    }

    ChartPanel panel;
    panel.xAxis = ChartPanel::Date;
    panel.yLabel = "Stress";
    panel.yRightLabel = "TSB";
    panel.series << stress << ctl << atl << tsb;

    ChartSpec spec;
    spec.title = QString("Performance manager (%1)").arg(RideMetricFactory::instance().rideMetric(metric)->name());
    spec.panels << panel;

    QJsonObject data;
    data.insert("metric", metric);
    data.insert("from", from.toString(Qt::ISODate));
    data.insert("to", to.toString(Qt::ISODate));
    return renderResult(spec, request, "pmc", data);
}

static CommandResult
zonesChart(CommandEnvironment &env, const CommandRequest &request)
{
    QString error;
    RideItem *item = env.session->findActivity(request.args.value("activity").toString(), error);
    if (!item) return CommandResult::failure(Status::NotFound, error);

    QString type = request.args.value("type").toString();
    ActivityZones zones;
    if (!activityZones(env.session->athlete(), item, type, zones, error)) return CommandResult::failure(Status::Failed, error);

    QStringList names;
    ChartSeries bars;
    bars.style = ChartSeries::Bars;
    bars.name = "Time in zone";
    bars.color = type == "hr" ? GColor(CHEARTRATE) : type == "pace" ? GColor(CSPEED)
               : type == "fatigue" ? GColor(CWBAL) : GColor(CPOWER);
    for (int z = 0; z < zones.rows.count(); z++) {
        names << zones.rows[z].name;
        bars.x << z;
        bars.y << zones.rows[z].seconds / 60.0;
    }

    ChartPanel panel;
    panel.xAxis = ChartPanel::Categories;
    panel.categories = names;
    panel.yLabel = "Minutes";
    panel.yMin = 0;
    panel.legend = false;
    panel.series << bars;

    ChartSpec spec;
    spec.title = QString("%1 time in zone, %2").arg(type == "hr" ? "Heart rate" : type == "pace" ? "Pace" : type == "fatigue" ? "W' balance" : "Power").arg(activityStart(item).replace("T", " "));
    spec.panels << panel;

    QJsonObject data;
    data.insert("activity", QFileInfo(item->fileName).completeBaseName());
    return renderResult(spec, request, QFileInfo(item->fileName).completeBaseName() + "-zones", data);
}

static CommandResult
trendChart(CommandEnvironment &env, const CommandRequest &request)
{
    QString metric = request.args.value("metric").toString();
    const RideMetric *m = RideMetricFactory::instance().rideMetric(metric);
    if (!m) return CommandResult::failure(Status::Usage, QString("unknown metric '%1'").arg(metric));

    QList<RideItem*> items;
    QString error;
    Status status;
    if (!selectedFiles(*env.session, request.args, items, error, status)) return CommandResult::failure(status, error);
    if (items.isEmpty()) return CommandResult::failure(Status::Failed, "no activities chosen");

    QString by = request.args.value("by").toString();
    bool average = m->type() == RideMetric::Average || m->type() == RideMetric::Peak || m->type() == RideMetric::Low;

    // bucket by period start, by activity every activity is its own bucket
    QMap<QDateTime, QPair<double,int>> buckets;
    for (RideItem *item : items) {
        QDate d = item->dateTime.date();
        QDateTime key = by == "activity" ? item->dateTime : QDateTime(d, QTime(0, 0));
        if (by == "week") key = QDateTime(d.addDays(1 - d.dayOfWeek()), QTime(0, 0));
        else if (by == "month") key = QDateTime(QDate(d.year(), d.month(), 1), QTime(0, 0));
        else if (by == "year") key = QDateTime(QDate(d.year(), 1, 1), QTime(0, 0));
        double v = item->getForSymbol(metric);
        if (!std::isfinite(v)) continue;
        auto &b = buckets[key];
        if (m->type() == RideMetric::Peak) b.first = std::max(b.first, v);
        else b.first += v;
        b.second++;
    }

    ChartSeries bars;
    bars.name = m->name();
    bars.style = by == "activity" ? ChartSeries::Dots : ChartSeries::Bars;
    bars.color = GColor(CPOWER);
    QStringList labels;
    int i = 0;
    for (auto it = buckets.constBegin(); it != buckets.constEnd(); ++it, ++i) {
        double v = it.value().first;
        if (average && m->type() != RideMetric::Peak && it.value().second) v /= it.value().second;
        bars.x << i;
        bars.y << v;
        QDate day = it.key().date();
        QString label = by == "month" ? day.toString("MMM yy") : by == "year" ? day.toString("yyyy") : day.toString("d MMM yy");
        labels << label;
    }

    // thin the labels out so they don't overlap
    int every = std::max(1, int(labels.count() / 24) + 1);
    for (int j = 0; j < labels.count(); j++) if (j % every) labels[j].clear();

    ChartPanel panel;
    panel.xAxis = ChartPanel::Categories;
    panel.categories = labels;
    panel.yLabel = m->units(true).isEmpty() ? m->name() : QString("%1 (%2)").arg(m->name()).arg(m->units(true));
    panel.yMin = 0;
    panel.legend = false;
    panel.series << bars;

    ChartSpec spec;
    spec.title = QString("%1 by %2").arg(m->name()).arg(by);
    spec.panels << panel;

    QJsonObject data;
    data.insert("metric", metric);
    data.insert("periods", bars.x.count());
    data.insert("activities", items.count());
    return renderResult(spec, request, "trend-" + metric, data);
}

void
registerChartCommands(CommandRegistry &registry)
{
    Command activity;
    activity.spec.name = "chart.activity";
    activity.spec.summary = "draw an activity's data series (power, heart rate, cadence ...)";
    activity.spec.scope = Scope::Athlete;
    activity.spec.params << ParamSpec("activity", ParamType::String, "activity id, start time, date, 'first' or 'last'").req().pos();
    activity.spec.params << ParamSpec("series", ParamType::String,
                                      "series to draw (default: all present of watts, hr, cad, speed, alt)").many();
    activity.spec.params << ParamSpec("smooth", ParamType::Int, "moving average in samples").def(1);
    activity.spec.params << ParamSpec("distance", ParamType::Bool, "distance on the x axis instead of time");
    activity.spec.params << imageParams();
    activity.spec.httpMethod = "GET";
    activity.spec.httpPath = "/athletes/{athlete}/activities/{activity}/chart";
    activity.handler = activityChart;
    registry.add(activity);

    Command mm;
    mm.spec.name = "chart.meanmax";
    mm.spec.summary = "draw the power-duration curve with a critical power model fit";
    mm.spec.scope = Scope::Athlete;
    mm.spec.params << ActivitySelection::params(true);
    QStringList models = modelNames();
    models << "none";
    mm.spec.params << ParamSpec("model", ParamType::String, "model to fit").def("cp3").oneOf(models);
    mm.spec.params << imageParams();
    mm.spec.httpMethod = "GET";
    mm.spec.httpPath = "/athletes/{athlete}/charts/meanmax";
    mm.handler = meanMaxChart;
    registry.add(mm);

    Command pmc;
    pmc.spec.name = "chart.pmc";
    pmc.spec.summary = "draw the performance manager chart (CTL, ATL, TSB)";
    pmc.spec.scope = Scope::Athlete;
    pmc.spec.params << ParamSpec("metric", ParamType::String, "stress metric").def("coggan_tss");
    pmc.spec.params << ParamSpec("from", ParamType::Date, "first day (default: 180 days before --to)");
    pmc.spec.params << ParamSpec("to", ParamType::Date, "last day (default: today)");
    pmc.spec.params << imageParams();
    pmc.spec.httpMethod = "GET";
    pmc.spec.httpPath = "/athletes/{athlete}/charts/pmc";
    pmc.handler = pmcChart;
    registry.add(pmc);

    Command zones;
    zones.spec.name = "chart.zones";
    zones.spec.summary = "draw time in power, heart rate, pace or W' balance zones for an activity";
    zones.spec.scope = Scope::Athlete;
    zones.spec.params << ParamSpec("activity", ParamType::String, "activity id, start time, date, 'first' or 'last'").req().pos();
    zones.spec.params << ParamSpec("type", ParamType::String, "zones").def("power").oneOf(zoneTypes());
    zones.spec.params << imageParams();
    zones.spec.httpMethod = "GET";
    zones.spec.httpPath = "/athletes/{athlete}/activities/{activity}/zones/chart";
    zones.handler = zonesChart;
    registry.add(zones);

    Command trend;
    trend.spec.name = "chart.trend";
    trend.spec.summary = "draw a metric over time, by activity, week, month or year";
    trend.spec.scope = Scope::Athlete;
    trend.spec.params << ParamSpec("metric", ParamType::String, "metric symbol").req().pos();
    trend.spec.params << ParamSpec("by", ParamType::String, "period").def("week").oneOf({ "activity", "day", "week", "month", "year" });
    trend.spec.params << ActivitySelection::params(false);
    trend.spec.params << imageParams();
    trend.spec.httpMethod = "GET";
    trend.spec.httpPath = "/athletes/{athlete}/charts/trend";
    trend.handler = trendChart;
    registry.add(trend);
}

} // namespace Headless
