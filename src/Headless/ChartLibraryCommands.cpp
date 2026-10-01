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
// Each chart is an LTMSettings blob written by LTMChartParser. A curve is a
// metric, a best (a duration of one series) or an estimate from a CP model
// (ChartCurves.h). Drawing flags belong to that one curve. Reading the file
// leaves out metric curves whose metric is not defined any more, so writing
// it would lose them: that is refused unless --drop-unknown says to.
//

#include "HeadlessCommands.h"
#include "ChartCurves.h"

#include "Athlete.h"
#include "Context.h"
#include "LTMChartParser.h"
#include "LTMSettings.h"

#include <QXmlInputSource>
#include <QXmlSimpleReader>

namespace Headless {

static QString
chartsPath(const Athlete *athlete)
{
    return LTMSettings::chartsFile(athlete->home->config());
}

static QJsonObject
chartJson(const LTMSettings &chart)
{
    QJsonArray metrics;
    for (int i = 0; i < chart.metrics.count(); i++) metrics.append(curveJson(chart.metrics.at(i), i + 1));
    QJsonObject o;
    o.insert("name", chart.name);
    o.insert("by", groups().name(chart.groupBy));
    o.insert("metrics", metrics);
    return o;
}

static QString
chartText(const LTMSettings &chart)
{
    QString text = QString("%1 (%2)\n").arg(chart.name, groups().name(chart.groupBy));
    for (int i = 0; i < chart.metrics.count(); i++) {
        const MetricDetail &m = chart.metrics.at(i);
        text += QString("  %1  %2  %3  %4  %5\n")
            .arg(i + 1)
            .arg(curveTypes().name(m.type))
            .arg(curveDetail(m))
            .arg(styles().name(m.curveStyle))
            .arg(markers().name(m.symbolStyle));
    }
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

static LTMSettings
blankChart(const QString &name, int groupBy)
{
    // the constructor leaves the flags unset (dates, fields and pointers
    // start empty)
    LTMSettings chart;
    chart.name = name;
    chart.title = name;
    chart.groupBy = groupBy;
    chart.shadeZones = false;
    chart.showData = false;
    chart.legend = false;
    chart.events = false;
    chart.stack = false;
    chart.stackWidth = 3;
    return chart;
}

// the charts as XML, read back to check nothing is lost in the file: an
// internal check (curves are built from what reading accepts, the unknown
// metrics are caught before), the XML is what gets saved
static bool
roundTrip(const QList<LTMSettings> &charts, QString &xml, QString &error)
{
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
            if (!sameCurve(want.metrics.at(j), got.metrics.at(j))) {
                error = QString("curve '%1' on '%2' could not be read back")
                    .arg(curveDetail(want.metrics.at(j)), want.name);
                return false;
            }
        }
    }
    return true;
}

static CommandResult
writeCharts(Athlete *athlete, QList<LTMSettings> charts, const QJsonObject &args)
{
    QStringList lost;
    for (LTMSettings &chart : charts) {
        if (chart.unknownMetrics.isEmpty()) continue;
        lost << QString("'%1' (%2)").arg(chart.name, chart.unknownMetrics.join(", "));
        chart.unknownMetrics.clear();
    }
    if (!lost.isEmpty() && !args.value("drop-unknown").toBool())
        return CommandResult::failure(Status::Failed,
                    QString("saving the charts would drop curves whose metric is not defined: %1. "
                            "Define the metrics again, or pass --drop-unknown").arg(lost.join("; ")));

    QString error, xml;
    if (!roundTrip(charts, xml, error)) return CommandResult::failure(Status::Failed, error);
    if (!athlete->saveCharts(charts, &error, &xml)) return CommandResult::failure(Status::Failed, error);
    return CommandResult::success();
}

// a change saved, and the chart it leaves (data) reported with its status
static CommandResult
saveChartChange(Athlete *athlete, const QList<LTMSettings> &charts, const QJsonObject &args,
                QJsonObject data, const QString &status, const QString &text)
{
    CommandResult written = writeCharts(athlete, charts, args);
    if (!written.ok()) return written;
    data.insert("status", status);
    data.insert("file", chartsPath(athlete));
    CommandResult result = CommandResult::success(data);
    result.text = text;
    return result;
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

static bool
replacementCurves(Context *context, const QJsonObject &args, QList<MetricDetail> &curves, QString &error)
{
    int sources = curveSources(args);
    if (sources == 0) {
        error = "give a curve with --metric, --best or --estimate";
        return false;
    }
    if (sources > 1) {
        error = "give only one of --metric, --best and --estimate";
        return false;
    }
    QStringList symbols;
    if (!curveMetrics(args, symbols, error)) return false;
    if (args.contains("metric") && symbols.count() != 1) {
        if (hasDrawing(args)) {
            error = oneCurveDrawing;
            return false;
        }
        if (!strayCurveArgs(args, error)) return false;
        return metricsFromSymbols(symbols, curves, error);
    }
    MetricDetail detail;
    if (!buildCurve(context, args, symbols, 0, detail, error)) return false;
    curves << detail;
    return true;
}

static CommandResult
addChart(CommandEnvironment &env, const CommandRequest &request)
{
    QString name = request.args.value("name").toString().trimmed();
    if (name.isEmpty()) return CommandResult::failure(Status::Usage, "name is empty");

    Athlete *athlete = env.session->athlete();
    if (nameTaken(athlete->presets, name, -1))
        return CommandResult::failure(Status::Usage, QString("a chart called '%1' already exists").arg(name));

    QList<MetricDetail> curves;
    QString error;
    if (!supportedTypes(request.args, error) || !replacementCurves(env.session->context(), request.args, curves, error))
        return CommandResult::failure(Status::Usage, error);

    QList<LTMSettings> charts = athlete->presets;
    LTMSettings chart = blankChart(name, groups().value(request.args.value("by").toString()));
    chart.metrics = curves;
    charts.append(chart);
    return saveChartChange(athlete, charts, request.args, chartJson(chart), "added", QString("added %1\n").arg(name));
}

static CommandResult
editChart(CommandEnvironment &env, const CommandRequest &request)
{
    bool rename = request.args.contains("name");
    bool regroup = request.args.contains("by");
    bool replace = curveSources(request.args) > 0;
    bool drawing = hasDrawing(request.args);
    QString error;
    if (!supportedTypes(request.args, error)) return CommandResult::failure(Status::Usage, error);
    if (!rename && !regroup && !replace && !drawing)
        return CommandResult::failure(Status::Usage, "give --name, --by, or a curve with --metric, --best or --estimate");
    if (drawing && !replace)
        return CommandResult::failure(Status::Usage, "to change one curve, use 'chart library curve edit'");
    // a curve's own flags without a curve would be dropped without a word
    if (!replace && !strayCurveArgs(request.args, error)) return CommandResult::failure(Status::Usage, error);

    Athlete *athlete = env.session->athlete();
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
    if (regroup) chart.groupBy = groups().value(request.args.value("by").toString());
    if (replace) {
        QList<MetricDetail> curves;
        if (!replacementCurves(env.session->context(), request.args, curves, error))
            return CommandResult::failure(Status::Usage, error);
        chart.metrics = curves;
    }
    return saveChartChange(athlete, charts, request.args, chartJson(chart), "updated",
                           QString("updated %1\n").arg(chart.name));
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
    return saveChartChange(athlete, charts, request.args, QJsonObject{ { "name", name } }, "removed",
                           QString("removed %1\n").arg(name));
}

static int
curveNumber(const LTMSettings &chart, int number, QString &error)
{
    if (number < 1 || number > chart.metrics.count()) {
        error = QString("no curve %1 on '%2'").arg(number).arg(chart.name);
        return -1;
    }
    return number - 1;
}

static CommandResult
addCurve(CommandEnvironment &env, const CommandRequest &request)
{
    QString error;
    if (!supportedTypes(request.args, error)) return CommandResult::failure(Status::Usage, error);
    if (curveSources(request.args) != 1)
        return CommandResult::failure(Status::Usage, "give one curve with --metric, --best or --estimate");

    Athlete *athlete = env.session->athlete();
    int chartIndex = findChart(athlete->presets, request.args.value("chart").toString(), error);
    if (chartIndex < 0) return CommandResult::failure(Status::Usage, error);

    QList<LTMSettings> charts = athlete->presets;
    LTMSettings &chart = charts[chartIndex];
    QStringList symbols;
    MetricDetail detail;
    if (!curveMetrics(request.args, symbols, error)
        || !buildCurve(env.session->context(), request.args, symbols, chart.metrics.count(), detail, error))
        return CommandResult::failure(Status::Usage, error);
    chart.metrics.append(detail);
    return saveChartChange(athlete, charts, request.args, chartJson(chart), "added",
                           QString("added curve %1 on %2\n").arg(chart.metrics.count()).arg(chart.name));
}

static CommandResult
editCurve(CommandEnvironment &env, const CommandRequest &request)
{
    QString error;
    if (!supportedTypes(request.args, error)) return CommandResult::failure(Status::Usage, error);

    Athlete *athlete = env.session->athlete();
    int chartIndex = findChart(athlete->presets, request.args.value("chart").toString(), error);
    if (chartIndex < 0) return CommandResult::failure(Status::Usage, error);

    QList<LTMSettings> charts = athlete->presets;
    LTMSettings &chart = charts[chartIndex];
    int index = curveNumber(chart, request.args.value("index").toInt(), error);
    if (index < 0) return CommandResult::failure(Status::Usage, error);

    int sources = curveSources(request.args);
    if (sources > 1)
        return CommandResult::failure(Status::Usage, "give only one of --metric, --best and --estimate");
    if (sources == 1) {
        // an estimate stays per kilogram, or not, unless --wpk says
        QJsonObject args = request.args;
        const MetricDetail &old = chart.metrics.at(index);
        if (args.contains("estimate") && !args.contains("wpk") && old.type == METRIC_ESTIMATE) args.insert("wpk", old.wpk);
        QStringList symbols;
        MetricDetail detail;
        if (!curveMetrics(args, symbols, error) || !buildCurve(env.session->context(), args, symbols, index, detail, error))
            return CommandResult::failure(Status::Usage, error);
        chart.metrics[index] = detail;
    } else if (hasDrawing(request.args)) {
        if (!strayCurveArgs(request.args, error)) return CommandResult::failure(Status::Usage, error);
        Drawing drawing;
        if (!readDrawing(env.session->context(), request.args, drawing, error))
            return CommandResult::failure(Status::Usage, error);
        applyDrawing(chart.metrics[index], drawing);
    } else {
        return CommandResult::failure(Status::Usage, "give a drawing or a curve to put in its place");
    }
    return saveChartChange(athlete, charts, request.args, chartJson(chart), "updated",
                           QString("updated curve %1 on %2\n").arg(index + 1).arg(chart.name));
}

static CommandResult
removeCurve(CommandEnvironment &env, const CommandRequest &request)
{
    Athlete *athlete = env.session->athlete();
    QString error;
    int chartIndex = findChart(athlete->presets, request.args.value("chart").toString(), error);
    if (chartIndex < 0) return CommandResult::failure(Status::Usage, error);

    QList<LTMSettings> charts = athlete->presets;
    LTMSettings &chart = charts[chartIndex];
    int index = curveNumber(chart, request.args.value("index").toInt(), error);
    if (index < 0) return CommandResult::failure(Status::Usage, error);
    chart.metrics.removeAt(index);
    return saveChartChange(athlete, charts, request.args, chartJson(chart), "removed",
                           QString("removed curve %1 on %2\n").arg(index + 1).arg(chart.name));
}

static ParamSpec
byParam(bool withDefault)
{
    ParamSpec by("by", ParamType::String, "group activities by " + groups().either());
    by.oneOf(groups().names);
    if (withDefault) by.def("week");
    return by;
}

// every command that writes the file
static void
writeParams(CommandSpec &spec)
{
    spec.params << ParamSpec("drop-unknown", ParamType::Bool,
                             "save even though curves whose metric is not defined are lost");
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
    show.spec.summary = "show one Trends chart and the curves it plots";
    show.spec.scope = Scope::Athlete;
    show.spec.params << ParamSpec("name", ParamType::String, "chart name").req().pos();
    show.spec.httpMethod = "GET";
    show.spec.httpPath = "/athletes/{athlete}/charts/{name}";
    show.handler = showChart;
    registry.add(show);

    Command add;
    add.spec.name = "chart.library.add";
    add.spec.summary = "add a Trends chart";
    add.spec.description =
        "A curve is a metric from 'metric list', a best (a duration of one series)\n"
        "or an estimate from a CP model (" + modelEither() + "). Several --metric flags\n"
        "and no drawing flags keep today's metric curves. A style, marker, color,\n"
        "fill, filter or units applies to one curve; add further curves with\n"
        "'chart library curve add'. --by defaults to week. PMC, Banister,\n"
        "performance, formula and measure curves are refused. A chart that cannot\n"
        "be read back is refused and the file is left unchanged.";
    add.spec.scope = Scope::Athlete;
    add.spec.modifies = true;
    add.spec.params << ParamSpec("name", ParamType::String, "name shown in the Trends sidebar").req();
    typedCurveParams(add.spec, true);
    add.spec.params << byParam(true);
    add.spec.httpMethod = "POST";
    add.spec.httpPath = "/athletes/{athlete}/charts";
    writeParams(add.spec);
    add.handler = addChart;
    registry.add(add);

    Command edit;
    edit.spec.name = "chart.library.edit";
    edit.spec.summary = "rename a Trends chart, replace its curves, or change how it groups";
    edit.spec.description =
        "Give --name, --by, or a curve. --metric, --best or --estimate replaces the\n"
        "whole curve list. To change one curve and leave the others, including bests\n"
        "and estimates already in the file, use 'chart library curve edit'.\n"
        "A change that cannot be read back is refused and the file is left unchanged.";
    edit.spec.scope = Scope::Athlete;
    edit.spec.modifies = true;
    edit.spec.params << ParamSpec("chart", ParamType::String, "chart to edit").req().pos();
    edit.spec.params << ParamSpec("name", ParamType::String, "new name");
    typedCurveParams(edit.spec, true);
    edit.spec.params << byParam(false);
    edit.spec.httpMethod = "PUT";
    edit.spec.httpPath = "/athletes/{athlete}/charts/{chart}";
    writeParams(edit.spec);
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
    writeParams(remove.spec);
    remove.handler = removeChart;
    registry.add(remove);

    Command curveAdd;
    curveAdd.spec.name = "chart.library.curve.add";
    curveAdd.spec.summary = "add one curve to a Trends chart";
    curveAdd.spec.description =
        "The new curve keeps its own type and drawing. The other curves are left\n"
        "as they are. A curve that cannot be read back is refused and the file is\n"
        "left unchanged.";
    curveAdd.spec.scope = Scope::Athlete;
    curveAdd.spec.modifies = true;
    curveAdd.spec.params << ParamSpec("chart", ParamType::String, "chart to add the curve to").req().pos();
    typedCurveParams(curveAdd.spec, false);
    curveAdd.spec.httpMethod = "POST";
    curveAdd.spec.httpPath = "/athletes/{athlete}/charts/{chart}/curves";
    writeParams(curveAdd.spec);
    curveAdd.handler = addCurve;
    registry.add(curveAdd);

    Command curveEdit;
    curveEdit.spec.name = "chart.library.curve.edit";
    curveEdit.spec.summary = "change one curve on a Trends chart";
    curveEdit.spec.description =
        "The curve number is the one 'chart library show' prints. Drawing flags\n"
        "that are left off stay as they are, and the other curves are not touched.\n"
        "Pass --metric, --best or --estimate to replace this curve.";
    curveEdit.spec.scope = Scope::Athlete;
    curveEdit.spec.modifies = true;
    curveEdit.spec.params << ParamSpec("chart", ParamType::String, "chart").req().pos();
    curveEdit.spec.params << ParamSpec("index", ParamType::Int, "curve number, starting at 1").req().pos();
    typedCurveParams(curveEdit.spec, false);
    curveEdit.spec.httpMethod = "PUT";
    curveEdit.spec.httpPath = "/athletes/{athlete}/charts/{chart}/curves/{index}";
    writeParams(curveEdit.spec);
    curveEdit.handler = editCurve;
    registry.add(curveEdit);

    Command curveRemove;
    curveRemove.spec.name = "chart.library.curve.remove";
    curveRemove.spec.summary = "remove one curve from a Trends chart";
    curveRemove.spec.scope = Scope::Athlete;
    curveRemove.spec.modifies = true;
    curveRemove.spec.params << ParamSpec("chart", ParamType::String, "chart").req().pos();
    curveRemove.spec.params << ParamSpec("index", ParamType::Int, "curve number, starting at 1").req().pos();
    curveRemove.spec.httpMethod = "DELETE";
    curveRemove.spec.httpPath = "/athletes/{athlete}/charts/{chart}/curves/{index}";
    writeParams(curveRemove.spec);
    curveRemove.handler = removeCurve;
    registry.add(curveRemove);
}


} // namespace Headless
