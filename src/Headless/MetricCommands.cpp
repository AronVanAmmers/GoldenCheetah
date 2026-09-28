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

#include "Context.h"
#include "Athlete.h"
#include "RideCache.h"
#include "RideItem.h"
#include "RideMetric.h"
#include "Specification.h"
#include "PMCData.h"
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

static QVector<double>
meanMaxOf(Context *context, const QList<RideItem*> &items, bool allRides, QDate from, QDate to,
          RideFile::SeriesType series)
{
    QStringList files;
    for (RideItem *i : items) files << i->fileName;

    if (items.count() == 1 && !allRides) {
        RideFileCache *cache = items.first()->fileCache();
        if (!cache) return QVector<double>();
        return cache->meanMaxArray(series);
    }
    if (!from.isValid()) from = QDate(1900, 1, 1);
    if (!to.isValid()) to = QDate(9999, 12, 31);
    RideFileCache bests(context, from, to, !allRides, files, true, nullptr);
    return bests.meanMaxArray(series);
}

QVector<double>
meanMax(AthleteSession &session, const QJsonObject &args, RideFile::SeriesType series,
        QString &error, Status &status, int &count)
{
    QList<RideItem*> items;
    if (!selectedFiles(session, args, items, error, status)) return QVector<double>();

    ActivitySelection s = ActivitySelection::fromArgs(args);
    // only an unfiltered date range may use the aggregate cache as is
    bool allRides = s.activities.isEmpty() && s.filter.isEmpty() && s.search.isEmpty()
                    && s.named.isEmpty() && s.sport.isEmpty() && s.limit == 0 && !s.planned;
    count = items.count();
    if (items.isEmpty()) return QVector<double>();
    return meanMaxOf(session.context(), items, allRides, s.from, s.to, series);
}

PDModel *
fitModel(Context *context, const QString &name, const QVector<double> &data)
{
    PDModel *model = nullptr;
    if (name == "cp2") model = new CP2Model(context);
    else if (name == "cp3") model = new CP3Model(context);
    else if (name == "extended") model = new ExtendedModel(context);
    else if (name == "multi") model = new MultiModel(context);
    else if (name == "ws") model = new WSModel(context);
    if (!model) return nullptr;

    // the critical power chart's default search intervals
    model->setFit(PDModel::Envelope);
    model->setIntervals(30, 60, 180, 360, 420, 1800, 3000, 30000);
    model->setMinutes(true);
    model->setData(data);
    return model;
}

QStringList modelNames() { return { "cp2", "cp3", "extended", "multi", "ws" }; }

RideFile::SeriesType
seriesFromName(const QString &name, bool &ok)
{
    ok = true;
    static const QMap<QString, RideFile::SeriesType> map = {
        { "watts", RideFile::watts }, { "power", RideFile::watts }, { "hr", RideFile::hr },
        { "cad", RideFile::cad }, { "speed", RideFile::kph }, { "nm", RideFile::nm },
        { "vam", RideFile::vam }, { "wpk", RideFile::wattsKg }, { "xpower", RideFile::xPower },
        { "isopower", RideFile::IsoPower }, { "apower", RideFile::aPower }
    };
    if (!map.contains(name.toLower())) { ok = false; return RideFile::watts; }
    return map.value(name.toLower());
}

QStringList seriesNames() { return { "watts", "hr", "cad", "speed", "nm", "vam", "wpk", "xpower", "isopower", "apower" }; }

PMCData *
pmcFor(AthleteSession &session, const QString &metric, int sts, int lts)
{
    // the athlete keeps these cached for charts, do the same
    return session.athlete()->getPMCFor(metric, sts, lts);
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
            && !m->name().contains(search, Qt::CaseInsensitive)) continue;
        QJsonObject o;
        o.insert("symbol", m->symbol());
        o.insert("name", m->name());
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
    for (const QString &symbol : splitList(request.args.value("metric"))) {
        if (!RideMetricFactory::instance().haveMetric(symbol))
            return CommandResult::failure(Status::Usage, QString("unknown metric '%1', see 'metric list'").arg(symbol));
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

static CommandResult
pmcCommand(CommandEnvironment &env, const CommandRequest &request)
{
    QString metric = request.args.value("metric").toString();
    if (!RideMetricFactory::instance().haveMetric(metric))
        return CommandResult::failure(Status::Usage, QString("unknown metric '%1', see 'metric list'").arg(metric));

    int sts = request.args.value("sts").toInt(-1);
    int lts = request.args.value("lts").toInt(-1);
    PMCData *pmc = pmcFor(*env.session, metric, sts, lts);
    if (!pmc) return CommandResult::failure(Status::Failed, "could not compute the performance manager data");

    QDate from = request.args.contains("from") ? QDate::fromString(request.args.value("from").toString(), Qt::ISODate) : pmc->start();
    QDate to = request.args.contains("to") ? QDate::fromString(request.args.value("to").toString(), Qt::ISODate) : QDate::currentDate();

    QJsonArray days;
    if (pmc->start().isValid()) {
        for (QDate d = from; d <= to; d = d.addDays(1)) {
            if (d < pmc->start() || d > pmc->end()) continue;
            QJsonObject o;
            o.insert("date", d.toString(Qt::ISODate));
            o.insert("stress", jsonNumber(pmc->stress(d)));
            o.insert("ctl", jsonNumber(pmc->lts(d)));
            o.insert("atl", jsonNumber(pmc->sts(d)));
            o.insert("tsb", jsonNumber(pmc->sb(d)));
            o.insert("rr", jsonNumber(pmc->rr(d)));
            days.append(o);
        }
    }
    QJsonObject data;
    data.insert("metric", metric);
    data.insert("sts_days", pmc->stsDays());
    data.insert("lts_days", pmc->ltsDays());
    data.insert("days", days);
    return CommandResult::success(data);
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
    env.session->waitForEstimates();

    QString sport = request.args.value("sport").toString();
    QString model = request.args.value("model").toString();
    QDate from = QDate::fromString(request.args.value("from").toString(), Qt::ISODate);
    QDate to = QDate::fromString(request.args.value("to").toString(), Qt::ISODate);

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
    list.spec.summary = "list metric symbols, names and units";
    list.spec.scope = Scope::Global;
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
    agg.spec.params << ParamSpec("metric", ParamType::String, "metric symbols (comma separated or repeated)").req().many();
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
    pmc.spec.params << ParamSpec("to", ParamType::Date, "last day (default: today)");
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
    est.spec.httpMethod = "GET";
    est.spec.httpPath = "/athletes/{athlete}/cp/estimates";
    est.handler = estimatesCommand;
    registry.add(est);
}

} // namespace Headless
