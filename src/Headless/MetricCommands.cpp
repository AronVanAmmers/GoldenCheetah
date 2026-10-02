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
// Metrics and models: the numbers behind the trends, PMC and critical power
// charts, computed from the saved activities the same way the GUI does.
//

#include "HeadlessCommands.h"
#include "MetricData.h"
#include "ActivitySelection.h"
#include "MetricNames.h"
#include "ActivityJson.h"
#include "SeasonRange.h"
#include "ResultFormat.h"

#include "Context.h"
#include "Athlete.h"
#include "RideCache.h"
#include "RideItem.h"
#include "RideMetric.h"
#include "Specification.h"
#include "PMCData.h"

#include <functional>
#include "PDModel.h"
#include "RideFileCache.h"
#include "Estimator.h"

#include <QFileInfo>
#include <cmath>

namespace Headless {

//
// MetricData.h
//

bool
selectedFiles(AthleteSession &session, const QJsonObject &args, QList<RideItem*> &items,
              QString &error, Status &status)
{
    ActivitySelection selection = ActivitySelection::fromArgs(args);
    return selection.resolve(session, items, error, status);
}

QJsonObject
powerSelection(const QJsonObject &args)
{
    // power-duration bests only make sense for one sport, cycling unless
    // activities are named or another sport is asked for
    QJsonObject out = args;
    if (!out.contains("sport") && out.value("activity").toArray().isEmpty()) out.insert("sport", "Bike");
    return out;
}

// bests over the activities chosen, from the activity's own cache for one.
// Always over the files chosen, never the cache of every activity: that
// mixes sports, and powerSelection always picks one unless activities are
// named.
QVector<double>
meanMax(AthleteSession &session, const QJsonObject &args, RideFile::SeriesType series,
        QString &error, Status &status, int &count)
{
    ActivitySelection s = ActivitySelection::fromArgs(args);
    QList<RideItem*> items;
    if (!s.resolve(session, items, error, status)) return QVector<double>();
    count = items.count();
    if (items.isEmpty()) return QVector<double>();

    if (items.count() == 1) {
        RideFileCache *cache = items.first()->fileCache();
        if (!cache) return QVector<double>();
        return cache->meanMaxArray(series);
    }
    QStringList files;
    for (RideItem *i : items) files << i->fileName;
    QDate from = s.from.isValid() ? s.from : QDate(1900, 1, 1);
    QDate to = s.to.isValid() ? s.to : QDate(9999, 12, 31);
    RideFileCache bests(session.context(), from, to, true, files, true, nullptr);
    return bests.meanMaxArray(series);
}

// the models cp fits, by the names --model takes
static const QList<QPair<QString, std::function<PDModel *(Context *)>>> &
modelTable()
{
    static const QList<QPair<QString, std::function<PDModel *(Context *)>>> models = {
        { "cp2", [](Context *c) -> PDModel * { return new CP2Model(c); } },
        { "cp3", [](Context *c) -> PDModel * { return new CP3Model(c); } },
        { "extended", [](Context *c) -> PDModel * { return new ExtendedModel(c); } },
        { "multi", [](Context *c) -> PDModel * { return new MultiModel(c); } },
        { "ws", [](Context *c) -> PDModel * { return new WSModel(c); } },
    };
    return models;
}

PDModel *
fitModel(Context *context, const QString &name, const QVector<double> &data)
{
    PDModel *model = nullptr;
    for (const auto &m : modelTable()) if (m.first == name) model = m.second(context);
    if (!model) return nullptr;

    // the critical power chart's default search intervals
    model->setFit(PDModel::Envelope);
    model->setIntervals(30, 60, 180, 360, 420, 1800, 3000, 30000);
    model->setMinutes(true);
    model->setData(data);
    return model;
}

QStringList
modelNames()
{
    QStringList names;
    for (const auto &m : modelTable()) names << m.first;
    return names;
}

// the series by the names --series takes; "power" is also watts, unlisted
struct SeriesName {
    const char *name;
    RideFile::SeriesType series;
    bool listed;
};

static const SeriesName seriesTable[] = {
    { "watts", RideFile::watts, true }, { "power", RideFile::watts, false }, { "hr", RideFile::hr, true },
    { "cad", RideFile::cad, true }, { "speed", RideFile::kph, true }, { "nm", RideFile::nm, true },
    { "vam", RideFile::vam, true }, { "wpk", RideFile::wattsKg, true }, { "xpower", RideFile::xPower, true },
    { "isopower", RideFile::IsoPower, true }, { "apower", RideFile::aPower, true },
};

RideFile::SeriesType
seriesFromName(const QString &name, bool &ok)
{
    ok = true;
    for (const SeriesName &s : seriesTable) if (name.toLower() == s.name) return s.series;
    ok = false;
    return RideFile::watts;
}

QStringList
seriesNames()
{
    QStringList names;
    for (const SeriesName &s : seriesTable) if (s.listed) names << s.name;
    return names;
}

PMCData *
pmcFor(AthleteSession &session, const QString &metric)
{
    // the athlete keeps these cached for charts, do the same
    return session.athlete()->getPMCFor(metric);
}

// the activities, actual and planned, that pass --sport and --filter, as
// file names; false (and nothing filtered) when neither is given
static bool
pmcMatches(AthleteSession &session, const QJsonObject &args, QStringList &files, QString &error, Status &status)
{
    files.clear();
    if (!args.contains("sport") && !args.contains("filter")) return false;
    QJsonObject only;
    if (args.contains("sport")) only.insert("sport", args.value("sport"));
    if (args.contains("filter")) only.insert("filter", args.value("filter"));
    for (bool planned : { false, true }) {
        ActivitySelection s = ActivitySelection::fromArgs(only);
        s.planned = planned;
        QList<RideItem *> items;
        if (!s.resolve(session, items, error, status)) return true;
        for (RideItem *i : items) files << i->fileName;
    }
    return true;
}

PMCData *
pmcForArgs(AthleteSession &session, const QJsonObject &args, const QString &metric,
           std::unique_ptr<PMCData> &owned, QString &error, Status &status)
{
    error.clear();
    int sts = args.value("sts").toInt(-1);
    int lts = args.value("lts").toInt(-1);
    QStringList files;
    bool filtered = pmcMatches(session, args, files, error, status);
    if (!error.isEmpty()) return nullptr;

    // the athlete's cached PMC has the athlete's time constants and no
    // filter (it is cached by metric only), others get a PMC of their own
    if (!filtered && sts < 0 && lts < 0) return pmcFor(session, metric);

    // as LTMPlot::createPMCData: the filter's matches, all dates
    Specification spec;
    if (filtered) spec.addMatches(files);
    spec.setDateRange(DateRange(QDate(), QDate()));
    owned.reset(new PMCData(session.context(), spec, metric, sts, lts));
    return owned.get();
}

QStringList
pmcSeriesNames(bool withAll)
{
    QStringList names = { "actual", "planned", "expected" };
    if (withAll) names << "all";
    return names;
}

PMCDay
pmcDay(PMCData *pmc, const QString &series, const QDate &d)
{
    PMCDay day;
    if (series == "planned") {
        day.stress = pmc->plannedStress(d);
        day.lts = pmc->plannedLts(d);
        day.sts = pmc->plannedSts(d);
        day.sb = pmc->plannedSb(d);
        day.rr = pmc->plannedRr(d);
    } else if (series == "expected") {
        day.stress = pmc->expectedStress(d);
        day.lts = pmc->expectedLts(d);
        day.sts = pmc->expectedSts(d);
        day.sb = pmc->expectedSb(d);
        day.rr = pmc->expectedRr(d);
    } else {
        day.stress = pmc->stress(d);
        day.lts = pmc->lts(d);
        day.sts = pmc->sts(d);
        day.sb = pmc->sb(d);
        day.rr = pmc->rr(d);
    }
    return day;
}

QDate
lastPlannedDay(AthleteSession &session, const QJsonObject &args)
{
    QStringList files;
    QString error;
    Status status;
    bool filtered = pmcMatches(session, args, files, error, status);
    QDate last;
    for (RideItem *item : session.rideCache()->rides()) {
        if (!item->planned) continue;
        if (filtered && !files.contains(item->fileName)) continue;
        if (!last.isValid() || item->dateTime.date() > last) last = item->dateTime.date();
    }
    return last;
}

QList<ParamSpec>
pmcParams(bool withAll)
{
    QList<ParamSpec> list;
    list << ParamSpec("series", ParamType::String,
                      withAll ? "actual (completed activities), planned, expected (completed until today, planned after), or all"
                              : "actual (completed activities), planned, or expected (completed until today, planned after)")
                .def("actual").oneOf(pmcSeriesNames(withAll));
    list << ParamSpec("sport", ParamType::String, "only activities of this sport (Bike, Run, Swim ...)");
    list << ParamSpec("filter", ParamType::String, "only activities that pass this filter, as typed in the GUI filter box");
    list << seasonParam();
    return list;
}

//
// Commands
//

static CommandResult
listMetrics(CommandEnvironment &, const CommandRequest &request)
{
    const RideMetricFactory &factory = RideMetricFactory::instance();
    QString search = request.args.value("search").toString();
    bool metricUnits = !request.args.value("imperial").toBool(false);

    QJsonArray list;
    for (int i = 0; i < factory.metricCount(); i++) {
        const RideMetric *m = factory.rideMetric(factory.metricName(i));
        if (!m) continue;
        if (!search.isEmpty() && !m->symbol().contains(search, Qt::CaseInsensitive)
            && !m->name().contains(search, Qt::CaseInsensitive)
            && !metricFormulaName(m->symbol()).contains(search, Qt::CaseInsensitive)) continue;
        QJsonObject o;
        o.insert("symbol", m->symbol());
        o.insert("name", m->name());
        o.insert("formula", metricFormulaName(m->symbol()));
        o.insert("units", m->units(metricUnits));
        o.insert("description", m->description());
        list.append(o);
    }
    QJsonObject data;
    data.insert("metrics", list);
    return CommandResult::success(data);
}

static CommandResult
aggregateMetrics(CommandEnvironment &env, const CommandRequest &request)
{
    QList<RideItem*> items;
    QString error;
    Status status;
    if (!selectedFiles(*env.session, request.args, items, error, status)) return CommandResult::failure(status, error);

    // a specification that passes exactly the selected activities
    QStringList files;
    for (RideItem *i : items) files << i->fileName;
    Specification spec;
    FilterSet fs;
    fs.addFilter(true, files);
    spec.setFilterSet(fs);

    bool metricUnits = !request.args.value("imperial").toBool(false);
    QJsonObject values;
    QStringList symbols;
    if (!resolveMetrics(splitList(request.args.value("metric")), symbols, error)) return CommandResult::failure(Status::Usage, error);
    for (const QString &symbol : symbols) {
        QString value = env.session->rideCache()->getAggregate(symbol, spec, metricUnits, true);
        bool ok = false;
        double d = value.toDouble(&ok);
        values.insert(symbol, ok ? jsonNumber(d) : QJsonValue(value));
    }
    QJsonObject data;
    data.insert("activities", items.count());
    data.insert("values", values);
    return CommandResult::success(data);
}

static QJsonObject
pmcDayJson(const PMCDay &day)
{
    QJsonObject o;
    o.insert("stress", jsonNumber(day.stress));
    o.insert("ctl", jsonNumber(day.lts));
    o.insert("atl", jsonNumber(day.sts));
    o.insert("tsb", jsonNumber(day.sb));
    o.insert("rr", jsonNumber(day.rr));
    return o;
}

static CommandResult
pmcCommand(CommandEnvironment &env, const CommandRequest &request)
{
    QString metric = metricSymbol(request.args.value("metric").toString());
    if (metric.isEmpty())
        return CommandResult::failure(Status::Usage, QString("unknown metric '%1', see 'metric list'").arg(request.args.value("metric").toString()));

    QString error;
    Status status = Status::Ok;
    QDate from, to;
    if (!dateRangeArgs(*env.session, request.args, from, to, error, status)) return CommandResult::failure(status, error);

    std::unique_ptr<PMCData> owned;
    PMCData *pmc = pmcForArgs(*env.session, request.args, metric, owned, error, status);
    if (!error.isEmpty()) return CommandResult::failure(status, error);
    if (!pmc) return CommandResult::failure(Status::Failed, "could not compute the performance manager data");

    // planned and expected look ahead, to the last planned day
    QString series = request.args.value("series").toString("actual");
    if (!from.isValid()) from = pmc->start();
    if (!to.isValid()) {
        to = QDate::currentDate();
        QDate planned = series == "actual" ? QDate() : lastPlannedDay(*env.session, request.args);
        if (planned.isValid() && planned > to) to = planned;
    }

    QJsonArray days;
    if (pmc->start().isValid()) {
        for (QDate d = from; d <= to; d = d.addDays(1)) {
            if (d < pmc->start() || d > pmc->end()) continue;
            QJsonObject o = pmcDayJson(pmcDay(pmc, series == "all" ? "actual" : series, d));
            if (series == "all") {
                o.insert("planned", pmcDayJson(pmcDay(pmc, "planned", d)));
                o.insert("expected", pmcDayJson(pmcDay(pmc, "expected", d)));
            }
            o.insert("date", d.toString(Qt::ISODate));
            days.append(o);
        }
    }
    QJsonObject data;
    data.insert("metric", metric);
    data.insert("series", series);
    data.insert("sts_days", pmc->stsDays());
    data.insert("lts_days", pmc->ltsDays());
    data.insert("days", days);
    CommandResult result = CommandResult::success(data);
    if (series == "all") {
        // each series' columns together, in the order the GUI names them
        QStringList order;
        for (const char *prefix : { "", "planned.", "expected." })
            for (const char *c : { "stress", "ctl", "atl", "tsb", "rr" }) order << QString(prefix) + c;
        result.text = ResultFormat::render(data, order);
        result.csv = ResultFormat::csv(data, order);
    }
    return result;
}

static CommandResult
meanMaxCommand(CommandEnvironment &env, const CommandRequest &request)
{
    bool ok = false;
    RideFile::SeriesType series = seriesFromName(request.args.value("series").toString(), ok);
    if (!ok) return CommandResult::failure(Status::Usage, "unknown series");

    QString error;
    Status status;
    int count = 0;
    QVector<double> data = meanMax(*env.session, powerSelection(request.args), series, error, status, count);
    if (!error.isEmpty()) return CommandResult::failure(status, error);

    // the standard durations, unless all were asked for
    QList<int> durations;
    if (request.args.value("every-second").toBool(false)) {
        for (int i = 1; i < data.count(); i++) durations << i;
    } else {
        durations = { 1, 5, 10, 15, 20, 30, 60, 120, 180, 300, 360, 480, 600, 900, 1200, 1800, 2400, 3600, 5400, 7200, 10800, 14400, 18000 };
    }

    QJsonArray points;
    for (int secs : durations) {
        if (secs >= data.count()) break;
        if (data[secs] <= 0) continue;
        QJsonObject o;
        o.insert("secs", secs);
        o.insert("value", jsonNumber(data[secs]));
        points.append(o);
    }
    QJsonObject out;
    out.insert("series", request.args.value("series").toString());
    out.insert("activities", count);
    out.insert("points", points);
    return CommandResult::success(out);
}

static CommandResult
cpCommand(CommandEnvironment &env, const CommandRequest &request)
{
    QString modelName = request.args.value("model").toString();
    QString error;
    Status status;
    int count = 0;
    QVector<double> data = meanMax(*env.session, powerSelection(request.args), RideFile::watts, error, status, count);
    if (!error.isEmpty()) return CommandResult::failure(status, error);
    if (data.count() < 2) return CommandResult::failure(Status::Failed, "no power data in the chosen activities");

    std::unique_ptr<PDModel> model(fitModel(env.session->context(), modelName, data));
    if (!model) return CommandResult::failure(Status::Usage, QString("unknown model '%1'").arg(modelName));

    QJsonObject out;
    out.insert("model", model->name());
    out.insert("code", model->code());
    out.insert("activities", count);
    if (model->hasCP()) out.insert("cp", jsonNumber(model->CP()));
    if (model->hasWPrime()) out.insert("wprime", jsonNumber(model->WPrime()));
    if (model->hasPMax()) out.insert("pmax", jsonNumber(model->PMax()));
    if (model->hasFTP()) out.insert("ftp", jsonNumber(model->FTP()));
    out.insert("summary", model->fitsummary.trimmed());
    return CommandResult::success(out);
}

static CommandResult
estimatesCommand(CommandEnvironment &env, const CommandRequest &request)
{
    QString sport = request.args.value("sport").toString();
    QString model = request.args.value("model").toString();
    QDate from, to;
    QString error;
    Status status = Status::Ok;
    if (!dateRangeArgs(*env.session, request.args, from, to, error, status)) return CommandResult::failure(status, error);

    env.session->waitForEstimates();

    QJsonArray list;
    for (const PDEstimate &e : env.session->athlete()->getPDEstimates()) {
        if (e.wpk) continue;
        if (!sport.isEmpty() && e.sport != sport) continue;
        if (!model.isEmpty() && e.model.compare(model, Qt::CaseInsensitive) != 0) continue;
        if (from.isValid() && e.to < from) continue;
        if (to.isValid() && e.from > to) continue;
        QJsonObject o;
        o.insert("from", e.from.toString(Qt::ISODate));
        o.insert("to", e.to.toString(Qt::ISODate));
        o.insert("model", e.model);
        o.insert("sport", e.sport);
        o.insert("cp", jsonNumber(e.CP));
        o.insert("wprime", jsonNumber(e.WPrime));
        o.insert("pmax", jsonNumber(e.PMax));
        o.insert("ftp", jsonNumber(e.FTP));
        list.append(o);
    }
    QJsonObject data;
    data.insert("estimates", list);
    return CommandResult::success(data);
}

void
registerMetricCommands(CommandRegistry &registry)
{
    Command list;
    list.spec.name = "metric.list";
    list.spec.summary = "list metrics, including this athlete's user metrics";
    list.spec.scope = Scope::Athlete;
    list.spec.params << ParamSpec("search", ParamType::String, "only metrics whose symbol or name contains this");
    list.spec.params << ParamSpec("imperial", ParamType::Bool, "show imperial units");
    list.spec.httpMethod = "GET";
    list.spec.httpPath = "/metrics";
    list.handler = listMetrics;
    registry.add(list);

    Command agg;
    agg.spec.name = "metric.aggregate";
    agg.spec.summary = "total or average metrics over activities, as the trends charts";
    agg.spec.scope = Scope::Athlete;
    agg.spec.params << ParamSpec("metric", ParamType::String, "metric symbols or formula names, e.g. Average_Power (comma separated or repeated)").req().many();
    agg.spec.params << ActivitySelection::params(false);
    agg.spec.params << ParamSpec("imperial", ParamType::Bool, "imperial units");
    agg.spec.httpMethod = "GET";
    agg.spec.httpPath = "/athletes/{athlete}/metrics/aggregate";
    agg.handler = aggregateMetrics;
    registry.add(agg);

    Command pmc;
    pmc.spec.name = "pmc";
    pmc.spec.summary = "performance manager: daily stress, CTL, ATL and TSB";
    pmc.spec.scope = Scope::Athlete;
    pmc.spec.params << ParamSpec("metric", ParamType::String, "stress metric").def("coggan_tss");
    pmc.spec.params << ParamSpec("from", ParamType::Date, "first day (default: first activity)");
    pmc.spec.params << ParamSpec("to", ParamType::Date, "last day (default: today, for planned and expected the last planned day if later)");
    pmc.spec.params << pmcParams(true);
    pmc.spec.params << ParamSpec("sts", ParamType::Int, "short term (ATL) days, default from the athlete settings");
    pmc.spec.params << ParamSpec("lts", ParamType::Int, "long term (CTL) days, default from the athlete settings");
    pmc.spec.httpMethod = "GET";
    pmc.spec.httpPath = "/athletes/{athlete}/pmc";
    pmc.handler = pmcCommand;
    registry.add(pmc);

    Command mm;
    mm.spec.name = "meanmax";
    mm.spec.summary = "best efforts (mean maximal values) for an activity or a date range";
    mm.spec.scope = Scope::Athlete;
    mm.spec.params << ParamSpec("series", ParamType::String, "data series").def("watts").oneOf(seriesNames());
    mm.spec.params << ActivitySelection::params(true);
    mm.spec.params << ParamSpec("every-second", ParamType::Bool, "every duration, not just the standard ones");
    mm.spec.httpMethod = "GET";
    mm.spec.httpPath = "/athletes/{athlete}/meanmax";
    mm.handler = meanMaxCommand;
    registry.add(mm);

    Command cp;
    cp.spec.name = "cp";
    cp.spec.summary = "fit a critical power model to the best efforts (CP, W', Pmax, FTP)";
    cp.spec.scope = Scope::Athlete;
    cp.spec.params << ParamSpec("model", ParamType::String, "power-duration model").def("cp3").oneOf(modelNames());
    cp.spec.params << ActivitySelection::params(true);
    cp.spec.httpMethod = "GET";
    cp.spec.httpPath = "/athletes/{athlete}/cp";
    cp.handler = cpCommand;
    registry.add(cp);

    Command est;
    est.spec.name = "cp.estimates";
    est.spec.summary = "the weekly model estimates the GUI uses for trends (CP, W', Pmax, FTP)";
    est.spec.scope = Scope::Athlete;
    est.spec.params << ParamSpec("sport", ParamType::String, "sport").def("Bike");
    est.spec.params << ParamSpec("model", ParamType::String, "only this model (by name)");
    est.spec.params << ParamSpec("from", ParamType::Date, "first week");
    est.spec.params << ParamSpec("to", ParamType::Date, "last week");
    est.spec.params << seasonParam();
    est.spec.httpMethod = "GET";
    est.spec.httpPath = "/athletes/{athlete}/cp/estimates";
    est.handler = estimatesCommand;
    registry.add(est);
}

} // namespace Headless
