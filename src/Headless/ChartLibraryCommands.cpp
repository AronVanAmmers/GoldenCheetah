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
// Trends sidebar charts, the ones stored in the athlete's config/charts.xml.
// Each chart is an LTMSettings blob written by LTMChartParser. A curve added
// here is one METRIC_DB metric. The file is only written after the chart
// list has been read back and still contains every curve.
//

#include "HeadlessCommands.h"
#include "ActivitySelection.h"

#include "Athlete.h"
#include "Context.h"
#include "LTMChartParser.h"
#include "LTMSettings.h"
#include "RideFile.h"
#include "RideMetric.h"
#include "Utils.h"

#include <QSaveFile>
#include <QXmlInputSource>
#include <QXmlSimpleReader>

namespace Headless {

static const QStringList groupNames = { "day", "week", "month", "year", "tod", "all" };

static int
groupId(const QString &name)
{
    switch (groupNames.indexOf(name)) {
    case 0: return LTM_DAY;
    case 1: return LTM_WEEK;
    case 2: return LTM_MONTH;
    case 3: return LTM_YEAR;
    case 4: return LTM_TOD;
    case 5: return LTM_ALL;
    default: return 0;
    }
}

static QString
groupName(int groupBy)
{
    switch (groupBy) {
    case LTM_DAY: return "day";
    case LTM_WEEK: return "week";
    case LTM_MONTH: return "month";
    case LTM_YEAR: return "year";
    case LTM_TOD: return "tod";
    case LTM_ALL: return "all";
    default: return QString::number(groupBy);
    }
}

static QString
curveTypeName(int type)
{
    switch (type) {
    case METRIC_DB: return "metric";
    case METRIC_PM: return "pmc";
    case METRIC_META: return "meta";
    case METRIC_BEST: return "best";
    case METRIC_ESTIMATE: return "estimate";
    case METRIC_STRESS: return "stress";
    case METRIC_FORMULA: return "formula";
    case METRIC_D_MEASURE: return "measure";
    case METRIC_PERFORMANCE: return "performance";
    case METRIC_BANISTER: return "banister";
    default: return QString::number(type);
    }
}

static QwtPlotCurve::CurveStyle
curveStyle(RideMetric::MetricType type)
{
    if (type == RideMetric::Total) return QwtPlotCurve::Steps;
    return QwtPlotCurve::Lines;
}

static QwtSymbol::Style
symbolStyle(RideMetric::MetricType type)
{
    switch (type) {
    case RideMetric::Average:
    case RideMetric::Total: return QwtSymbol::Ellipse;
    case RideMetric::Peak: return QwtSymbol::Rect;
    default: return QwtSymbol::XCross;
    }
}

static QColor
penColor(int index)
{
    static const QColor pens[] = {
        QColor(0x00, 0x78, 0xd4), QColor(0xe0, 0x4e, 0x39), QColor(0x1a, 0x7f, 0x37),
        QColor(0xb0, 0x6a, 0x00), QColor(0x6b, 0x4c, 0x9a), QColor(0x0e, 0x74, 0x90)
    };
    return pens[index % int(sizeof(pens) / sizeof(pens[0]))];
}

static QString
chartsPath(const Athlete *athlete)
{
    return athlete->home->config().canonicalPath() + "/charts.xml";
}

static QJsonObject
curveJson(const MetricDetail &m)
{
    QJsonObject o;
    o.insert("type", curveTypeName(m.type));
    o.insert("symbol", m.symbol);
    o.insert("formula", metricFormulaName(m.symbol));
    o.insert("name", m.name);
    return o;
}

static QJsonObject
chartJson(const LTMSettings &chart)
{
    QJsonArray metrics;
    for (const MetricDetail &m : chart.metrics) metrics.append(curveJson(m));
    QJsonObject o;
    o.insert("name", chart.name);
    o.insert("by", groupName(chart.groupBy));
    o.insert("metrics", metrics);
    return o;
}

static QString
chartText(const LTMSettings &chart)
{
    QString text = QString("%1 (%2)\n").arg(chart.name, groupName(chart.groupBy));
    for (const MetricDetail &m : chart.metrics)
        text += QString("  %1  %2\n").arg(m.symbol, m.name);
    return text;
}

static int
findChart(const QList<LTMSettings> &charts, const QString &name, QString &error)
{
    int found = -1;
    for (int i = 0; i < charts.count(); i++) {
        if (charts.at(i).name != name) continue;
        if (found >= 0) {
            error = QString("more than one chart is called '%1'").arg(name);
            return -1;
        }
        found = i;
    }
    if (found < 0) error = QString("no chart called '%1'").arg(name);
    return found;
}

static bool
nameTaken(const QList<LTMSettings> &charts, const QString &name, int except)
{
    for (int i = 0; i < charts.count(); i++)
        if (i != except && charts.at(i).name == name) return true;
    return false;
}

// the same fields LTMTool fills in when a metric is picked from its catalogue
static MetricDetail
metricCurve(const RideMetric *metric, int index)
{
    MetricDetail detail;
    detail.type = METRIC_DB;
    detail.symbol = metric->symbol();
    detail.metric = metric;
    detail.name = Utils::unprotect(metric->name());
    detail.uname = detail.name;
    bool metricUnits = GlobalContext::context()->useMetricUnits;
    detail.units = metric->units(metricUnits);
    detail.uunits = detail.units.isEmpty() ? detail.name : detail.units;
    detail.topN = 1;
    detail.curveStyle = curveStyle(metric->type());
    detail.symbolStyle = symbolStyle(metric->type());
    detail.penColor = penColor(index);
    detail.brushColor = detail.penColor;
    detail.showOnPlot = true;
    detail.filter = 0;
    detail.from = 0;
    detail.to = 0;
    detail.estimate = 0;
    detail.estimateDuration = 0;
    detail.estimateDuration_units = 1;
    detail.wpk = false;
    detail.run = false;
    detail.duration = 0;
    detail.duration_units = 1;
    detail.series = RideFile::none;
    detail.submax = false;
    detail.formulaType = metric->type();
    return detail;
}

static LTMSettings
blankChart(const QString &name, int groupBy)
{
    LTMSettings chart;
    chart.name = name;
    chart.title = name;
    chart.start = QDateTime();
    chart.end = QDateTime();
    chart.groupBy = groupBy;
    chart.shadeZones = false;
    chart.showData = false;
    chart.legend = false;
    chart.events = false;
    chart.stack = false;
    chart.stackWidth = 3;
    chart.field1.clear();
    chart.field2.clear();
    chart.bests = nullptr;
    chart.ltmTool = nullptr;
    return chart;
}

static bool
curvesFromArgs(const QJsonObject &args, QList<MetricDetail> &curves, QString &error)
{
    QStringList symbols;
    if (!resolveMetrics(splitList(args.value("metric")), symbols, error)) return false;
    if (symbols.isEmpty()) {
        error = "give a metric with --metric";
        return false;
    }
    const RideMetricFactory &factory = RideMetricFactory::instance();
    for (int i = 0; i < symbols.count(); i++) {
        if (symbols.at(i).startsWith("compatibility_")) {
            error = QString("metric '%1' is only kept for old charts, see 'metric list'").arg(symbols.at(i));
            return false;
        }
        const RideMetric *metric = factory.rideMetric(symbols.at(i));
        if (!metric) {
            error = QString("unknown metric '%1', see 'metric list'").arg(symbols.at(i));
            return false;
        }
        curves << metricCurve(metric, i);
    }
    return true;
}

// operator>> drops a METRIC_DB curve it does not recognise, without saying so
static bool
roundTrip(const QList<LTMSettings> &charts, QString &error)
{
    QString xml;
    LTMChartParser::serializeToQString(&xml, charts);

    QXmlInputSource source;
    source.setData(xml);
    QXmlSimpleReader reader;
    LTMChartParser handler;
    reader.setContentHandler(&handler);
    reader.setErrorHandler(&handler);
    if (!reader.parse(source)) {
        error = "chart could not be read back";
        return false;
    }

    const QList<LTMSettings> back = handler.getSettings();
    if (back.count() != charts.count()) {
        error = "chart could not be read back";
        return false;
    }
    for (int i = 0; i < charts.count(); i++) {
        const LTMSettings &want = charts.at(i);
        const LTMSettings &got = back.at(i);
        if (got.name != want.name || got.groupBy != want.groupBy || got.metrics.count() != want.metrics.count()) {
            error = QString("chart '%1' could not be read back").arg(want.name);
            return false;
        }
        for (int j = 0; j < want.metrics.count(); j++) {
            if (got.metrics.at(j).type != want.metrics.at(j).type || got.metrics.at(j).symbol != want.metrics.at(j).symbol) {
                error = QString("curve '%1' on '%2' could not be read back").arg(want.metrics.at(j).symbol, want.name);
                return false;
            }
        }
    }
    return true;
}

static CommandResult
writeCharts(Athlete *athlete, const QList<LTMSettings> &charts)
{
    QString error;
    if (!roundTrip(charts, error)) return CommandResult::failure(Status::Usage, error);

    QString xml;
    LTMChartParser::serializeToQString(&xml, charts);
    QString path = chartsPath(athlete);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return CommandResult::failure(Status::Failed, QString("can't write %1").arg(path));
    QByteArray bytes = xml.toUtf8();
    if (file.write(bytes) != bytes.size() || !file.commit())
        return CommandResult::failure(Status::Failed, QString("can't write %1").arg(path));

    athlete->presets = charts;
    athlete->presetsDirty = true;
    return CommandResult::success();
}

static CommandResult
listCharts(CommandEnvironment &env, const CommandRequest &)
{
    const QList<LTMSettings> &charts = env.session->athlete()->presets;
    QJsonArray list;
    QString text;
    for (const LTMSettings &chart : charts) {
        list.append(chartJson(chart));
        if (!text.isEmpty()) text += "\n";
        text += chartText(chart);
    }
    QJsonObject data;
    data.insert("charts", list);
    CommandResult result = CommandResult::success(data);
    result.text = text.isEmpty() ? "(none)\n" : text;
    return result;
}

static CommandResult
showChart(CommandEnvironment &env, const CommandRequest &request)
{
    QString error;
    int index = findChart(env.session->athlete()->presets, request.args.value("name").toString(), error);
    if (index < 0) return CommandResult::failure(Status::Usage, error);
    const LTMSettings &chart = env.session->athlete()->presets.at(index);
    CommandResult result = CommandResult::success(chartJson(chart));
    result.text = chartText(chart);
    return result;
}

static CommandResult
addChart(CommandEnvironment &env, const CommandRequest &request)
{
    QString name = request.args.value("name").toString().trimmed();
    if (name.isEmpty()) return CommandResult::failure(Status::Usage, "name is empty");

    Athlete *athlete = env.session->athlete();
    if (nameTaken(athlete->presets, name, -1))
        return CommandResult::failure(Status::Usage, QString("a chart called '%1' already exists").arg(name));

    int by = groupId(request.args.value("by").toString());
    if (!by) return CommandResult::failure(Status::Usage, "group by must be day, week, month, year, tod or all");

    QList<MetricDetail> curves;
    QString error;
    if (!curvesFromArgs(request.args, curves, error)) return CommandResult::failure(Status::Usage, error);

    QList<LTMSettings> charts = athlete->presets;
    LTMSettings chart = blankChart(name, by);
    chart.metrics = curves;
    charts.append(chart);

    CommandResult written = writeCharts(athlete, charts);
    if (!written.ok()) return written;

    QJsonObject data = chartJson(chart);
    data.insert("status", "added");
    data.insert("file", chartsPath(athlete));
    CommandResult result = CommandResult::success(data);
    result.text = QString("added %1\n").arg(name);
    return result;
}

static CommandResult
editChart(CommandEnvironment &env, const CommandRequest &request)
{
    bool rename = request.args.contains("name");
    bool remetric = request.args.contains("metric");
    bool regroup = request.args.contains("by");
    if (!rename && !remetric && !regroup)
        return CommandResult::failure(Status::Usage, "give --name, --metric or --by");

    Athlete *athlete = env.session->athlete();
    QString error;
    int index = findChart(athlete->presets, request.args.value("chart").toString(), error);
    if (index < 0) return CommandResult::failure(Status::Usage, error);

    QList<LTMSettings> charts = athlete->presets;
    LTMSettings &chart = charts[index];

    if (rename) {
        QString name = request.args.value("name").toString().trimmed();
        if (name.isEmpty()) return CommandResult::failure(Status::Usage, "name is empty");
        if (nameTaken(charts, name, index))
            return CommandResult::failure(Status::Usage, QString("a chart called '%1' already exists").arg(name));
        chart.name = name;
        chart.title = name;
    }
    if (regroup) {
        int by = groupId(request.args.value("by").toString());
        if (!by) return CommandResult::failure(Status::Usage, "group by must be day, week, month, year, tod or all");
        chart.groupBy = by;
    }
    if (remetric) {
        QList<MetricDetail> curves;
        if (!curvesFromArgs(request.args, curves, error)) return CommandResult::failure(Status::Usage, error);
        chart.metrics = curves;
    }

    CommandResult written = writeCharts(athlete, charts);
    if (!written.ok()) return written;

    QJsonObject data = chartJson(charts.at(index));
    data.insert("status", "updated");
    data.insert("file", chartsPath(athlete));
    CommandResult result = CommandResult::success(data);
    result.text = QString("updated %1\n").arg(charts.at(index).name);
    return result;
}

static CommandResult
removeChart(CommandEnvironment &env, const CommandRequest &request)
{
    Athlete *athlete = env.session->athlete();
    QString name = request.args.value("name").toString();
    QString error;
    int index = findChart(athlete->presets, name, error);
    if (index < 0) return CommandResult::failure(Status::Usage, error);

    QList<LTMSettings> charts = athlete->presets;
    charts.removeAt(index);
    CommandResult written = writeCharts(athlete, charts);
    if (!written.ok()) return written;

    QJsonObject data;
    data.insert("status", "removed");
    data.insert("name", name);
    data.insert("file", chartsPath(athlete));
    CommandResult result = CommandResult::success(data);
    result.text = QString("removed %1\n").arg(name);
    return result;
}

static ParamSpec
byParam(bool withDefault)
{
    ParamSpec by("by", ParamType::String, "group activities by day, week, month, year, tod or all");
    by.oneOf(groupNames);
    if (withDefault) by.def("week");
    return by;
}

void
registerChartLibraryCommands(CommandRegistry &registry)
{
    Command list;
    list.spec.name = "chart.library.list";
    list.spec.summary = "list the Trends charts saved for this athlete";
    list.spec.description =
        "These are the charts in the Trends sidebar, from config/charts.xml\n"
        "when the athlete has one, otherwise the built-in charts. Listing them\n"
        "does not create the file.";
    list.spec.scope = Scope::Athlete;
    list.spec.httpMethod = "GET";
    list.spec.httpPath = "/athletes/{athlete}/charts";
    list.handler = listCharts;
    registry.add(list);

    Command show;
    show.spec.name = "chart.library.show";
    show.spec.summary = "show one Trends chart and the metrics it plots";
    show.spec.scope = Scope::Athlete;
    show.spec.params << ParamSpec("name", ParamType::String, "chart name").req().pos();
    show.spec.httpMethod = "GET";
    show.spec.httpPath = "/athletes/{athlete}/charts/{name}";
    show.handler = showChart;
    registry.add(show);

    Command add;
    add.spec.name = "chart.library.add";
    add.spec.summary = "add a Trends chart with one or more metric curves";
    add.spec.description =
        "Each --metric is a metric from 'metric list', by symbol or formula name,\n"
        "drawn as GoldenCheetah stores a normal metric curve. --by defaults to week.\n"
        "A metric that is not known, or a chart that cannot be read back, is refused\n"
        "and the file is left unchanged.";
    add.spec.scope = Scope::Athlete;
    add.spec.modifies = true;
    add.spec.params << ParamSpec("name", ParamType::String, "name shown in the Trends sidebar").req();
    add.spec.params << ParamSpec("metric", ParamType::String, "metric symbol or formula name").req().many();
    add.spec.params << byParam(true);
    add.spec.httpMethod = "POST";
    add.spec.httpPath = "/athletes/{athlete}/charts";
    add.handler = addChart;
    registry.add(add);

    Command edit;
    edit.spec.name = "chart.library.edit";
    edit.spec.summary = "rename a Trends chart, replace its curves, or change how it groups";
    edit.spec.description =
        "Give at least one of --name, --metric and --by. --metric replaces the\n"
        "curves; leave it out to keep them. A change that cannot be read back is\n"
        "refused and the file is left unchanged.";
    edit.spec.scope = Scope::Athlete;
    edit.spec.modifies = true;
    edit.spec.params << ParamSpec("chart", ParamType::String, "chart to edit").req().pos();
    edit.spec.params << ParamSpec("name", ParamType::String, "new name");
    edit.spec.params << ParamSpec("metric", ParamType::String, "metric symbol or formula name, replacing the curves").many();
    edit.spec.params << byParam(false);
    edit.spec.httpMethod = "PUT";
    edit.spec.httpPath = "/athletes/{athlete}/charts/{chart}";
    edit.handler = editChart;
    registry.add(edit);

    Command remove;
    remove.spec.name = "chart.library.remove";
    remove.spec.summary = "remove a Trends chart";
    remove.spec.scope = Scope::Athlete;
    remove.spec.modifies = true;
    remove.spec.params << ParamSpec("name", ParamType::String, "chart name").req().pos();
    remove.spec.httpMethod = "DELETE";
    remove.spec.httpPath = "/athletes/{athlete}/charts/{name}";
    remove.handler = removeChart;
    registry.add(remove);
}

} // namespace Headless
