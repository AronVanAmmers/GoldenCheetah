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
// A Trends sidebar chart drawn as the Trends view draws it. The chart is
// prepared as LTMWindow prepares it (LTMSettings::prepare: dates, filters,
// bests), plotted by the GUI's own LTMPlot without a window and rendered
// with QwtPlotRenderer. Its data is the window's data table (LTMDataTable),
// computed from the same curve data.
//

#include "HeadlessCommands.h"
#include "TrendsChart.h"
#include "ChartCurves.h"
#include "ChartImage.h"
#include "ChartRenderer.h"
#include "ResultFormat.h"
#include "SeasonRange.h"

#include "Athlete.h"
#include "Context.h"
#include "LTMDataTable.h"
#include "LTMPlot.h"
#include "RideFileCache.h"
#include "SearchFilterBox.h"
#include "Season.h"
#include "Seasons.h"
#include "Settings.h"

#include <QFont>
#include <QRegularExpression>
#include <memory>

namespace Headless {

// the sidebar's "All Dates" season (Seasons::readSeasons)
static const QUuid allDatesId("{00000000-0000-0000-0000-000000000001}");

// a chart ready to plot, and the bests its curves need, which must outlive the plot
struct TrendsChart {
    LTMSettings settings;
    QList<RideBest> bests;
};

static DateRange
allDates(Context *context)
{
    for (const Season &season : context->athlete->seasons->seasons)
        if (season.id() == allDatesId) return DateRange(season.getStart(), season.getEnd(), season.getName());
    return DateRange(QDate(1900, 1, 1), QDate(2999, 12, 31), "All Dates");
}

// The dates to draw. With none given, the sidebar's All Dates season: the
// plot then runs from the first activity to the last one, planned ones
// included (LTMPlot::setData crops the season to them). The GUI opens on the
// season picked last, Last 3 months when none was.
//
// the dates the chart covers: a season (named, so the chart leaves it
// unmarked as the GUI does), --from/--to, or all dates
static bool
chartRange(CommandEnvironment &env, const QJsonObject &args, DateRange &range, QString &error, Status &status)
{
    if (args.contains("season")) {
        if (args.contains("from") || args.contains("to")) {
            error = "--season stands for --from and --to: give one or the other";
            status = Status::Usage;
            return false;
        }
        return seasonRange(*env.session, args.value("season").toString(), range, error, &status);
    }

    DateRange all = allDates(env.session->context());
    if (!args.contains("from") && !args.contains("to")) {
        range = all;
        return true;
    }

    // a range of the user's own, as Chart Setup's custom dates: no season name
    QDate from = args.contains("from") ? QDate::fromString(args.value("from").toString(), Qt::ISODate) : all.from;
    QDate to = args.contains("to") ? QDate::fromString(args.value("to").toString(), Qt::ISODate) : all.to;
    if (from > to) {
        error = QString("--from %1 is after --to %2").arg(from.toString(Qt::ISODate), to.toString(Qt::ISODate));
        status = Status::Usage;
        return false;
    }
    range = DateRange(from, to, QString());
    return true;
}

static bool
needsEstimates(const LTMSettings &settings)
{
    for (const MetricDetail &m : settings.metrics)
        if (m.type == METRIC_ESTIMATE || m.type == METRIC_PERFORMANCE) return true;
    return false;
}

// the chart named, with the dates, grouping and filter asked for, prepared
// as the Trends view prepares it
static bool
prepareChart(CommandEnvironment &env, const QJsonObject &args, TrendsChart &chart, Status &status, QString &error)
{
    Context *context = env.session->context();
    const QList<LTMSettings> &presets = env.session->athlete()->presets;

    QString name = args.value("name").toString();
    int found = 0;
    for (const LTMSettings &preset : presets) if (preset.name == name) found++;
    if (found != 1) {
        status = found ? Status::Usage : Status::NotFound;
        error = found ? QString("more than one chart is called '%1'").arg(name)
                      : QString("no chart called '%1', see 'chart library list'").arg(name);
        return false;
    }
    chart.settings = presets.at(findChart(presets, name, error));

    status = Status::Usage;
    DateRange range;
    if (!chartRange(env, args, range, error, status)) return false;
    status = Status::Usage;
    // a few built-in charts were saved with no valid grouping: the sidebar
    // never applies it (it keeps the view's), so group those by week
    if (args.contains("by")) chart.settings.groupBy = groups().value(args.value("by").toString());
    else if (!groups().values.contains(chart.settings.groupBy)) chart.settings.groupBy = LTM_WEEK;

    // an extra filter, as the GUI's filter box adds one to the chart's own
    FilterSet filters;
    if (args.contains("filter")) {
        QString expression = args.value("filter").toString();
        if (!checkFilter(context, expression, error)) {
            error = QString("bad filter '%1': %2").arg(expression, error);
            return false;
        }
        if (!expression.trimmed().isEmpty())
            filters.addFilter(true, SearchFilterBox::matches(context, "filter:" + expression));
    }

    // estimates are computed on demand, the GUI has them from the start
    if (needsEstimates(chart.settings)) env.session->waitForEstimates();

    chart.settings.title = range.name;
    chart.settings.prepare(context, range, false, filters, chart.bests);
    return true;
}

// the font the GUI draws charts in, from its appearance settings (main())
static QFont
guiFont()
{
    AppearanceSettings defaults = GSettings::defaultAppearanceSettings();
    QFont font;
    font.fromString(appsettings->value(NULL, GC_FONT_DEFAULT, defaults.fontfamily).toString());
    font.setPointSize(defaults.fontpointsize);
    double scale = appsettings->value(NULL, GC_FONT_SCALE, defaults.fontscale).toDouble();
    font.setPointSizeF(font.pointSizeF() * scale);
    return font;
}

static QJsonObject
chartSummary(const TrendsChart &chart)
{
    QJsonObject data;
    data.insert("name", chart.settings.name);
    data.insert("by", groups().name(chart.settings.groupBy));
    data.insert("from", chart.settings.start.date().toString(Qt::ISODate));
    data.insert("to", chart.settings.end.date().toString(Qt::ISODate));
    data.insert("curves", chart.settings.metrics.count());
    return data;
}

static CommandResult
renderChart(CommandEnvironment &env, const CommandRequest &request)
{
    TrendsChart chart;
    Status status;
    QString error;
    if (!prepareChart(env, request.args, chart, status, error)) return CommandResult::failure(status, error);

    QSize size = imageSize(request);
    QString format = request.args.value("as").toString("png");
    QString title = request.args.value("title").toString();

    std::unique_ptr<LTMPlot> plot(new LTMPlot(nullptr, env.session->context(), 0));
    plot->setFont(guiFont());
    plot->resize(size);
    plot->setData(&chart.settings);
    if (!title.isEmpty()) plot->setTitle(title);

    QByteArray bytes = ChartRenderer::renderPlot(plot.get(), size, format, title, error);
    QString file = chart.settings.name;
    file.replace(QRegularExpression("[^A-Za-z0-9_.-]+"), "_");
    return imageResult(bytes, error, request, file, chartSummary(chart));
}

static CommandResult
chartData(CommandEnvironment &env, const CommandRequest &request)
{
    TrendsChart chart;
    Status status;
    QString error;
    if (!prepareChart(env, request.args, chart, status, error)) return CommandResult::failure(status, error);

    // the plot's curve data, as the window's data table has it
    std::unique_ptr<LTMPlot> plot(new LTMPlot(nullptr, env.session->context(), 0));
    LTMDataTable table(env.session->context(), plot.get(), chart.settings);

    QJsonArray columns;
    QStringList headings = { table.dateHeading.isEmpty() ? QString("Date") : table.dateHeading };
    for (int i = 0; i < table.columns.count(); i++) {
        const LTMDataTable::Column &c = table.columns.at(i);
        const MetricDetail &m = chart.settings.metrics.at(i);
        QJsonObject o;
        o.insert("curve", i + 1);
        o.insert("name", c.name);
        o.insert("units", c.units);
        o.insert("type", curveTypes().name(m.type));
        o.insert("detail", curveDetail(m));
        columns.append(o);
        headings << (c.units.isEmpty() ? c.name : QString("%1 (%2)").arg(c.name, c.units));
    }

    QJsonArray rows;
    QList<QStringList> lines;
    for (const LTMDataTable::Row &r : table.rows) {
        QJsonArray values;
        for (double v : r.values) values.append(v);
        QJsonObject o;
        o.insert("date", r.date);
        o.insert("label", r.label);
        o.insert("values", values);
        QJsonArray text;
        for (const QString &t : r.text) text.append(t);
        o.insert("text", text);
        rows.append(o);
        lines << (QStringList() << r.date << r.text);
    }

    QJsonObject data = chartSummary(chart);
    data.insert("columns", columns);
    data.insert("rows", rows);
    CommandResult result = CommandResult::success(data);

    // the table as Export Chart Data has it, a column per curve
    result.csv = ResultFormat::csvLine(headings);
    for (const QStringList &line : lines) result.csv += ResultFormat::csvLine(line);

    QVector<int> widths(headings.count());
    for (int c = 0; c < headings.count(); c++) widths[c] = headings[c].length();
    for (const QStringList &line : lines)
        for (int c = 0; c < line.count() && c < widths.count(); c++) widths[c] = std::max(widths[c], int(line[c].length()));
    auto textLine = [&widths](const QStringList &cells) {
        QString line;
        for (int c = 0; c < cells.count(); c++) {
            if (c) line += "  ";
            // the date on the left, numbers on the right
            line += c == 0 ? cells[c].leftJustified(widths[c]) : cells[c].rightJustified(widths[c]);
        }
        return line.trimmed() + "\n";
    };
    QString text = QString("%1, %2 to %3, by %4\n")
        .arg(chart.settings.name, data.value("from").toString(), data.value("to").toString(), data.value("by").toString());
    if (lines.isEmpty()) text += "(no data)\n";
    else {
        text += textLine(headings);
        for (const QStringList &line : lines) text += textLine(line);
    }
    result.text = text;
    return result;
}

static void
chartParams(CommandSpec &spec)
{
    spec.params << ParamSpec("name", ParamType::String, "chart name, from 'chart library list'").req().pos();
    spec.params << ParamSpec("from", ParamType::Date, "first day (default: all dates)");
    spec.params << ParamSpec("to", ParamType::Date, "last day (default: all dates)");
    spec.params << seasonParam();
    ParamSpec by("by", ParamType::String, "group by " + groups().either() + " (default: the chart's own)");
    by.oneOf(groups().names);
    spec.params << by;
    spec.params << ParamSpec("filter", ParamType::String, "only activities that pass this filter, as the GUI filter box");
}

void
registerTrendsChartCommands(CommandRegistry &registry)
{
    Command render;
    render.spec.name = "chart.library.render";
    render.spec.summary = "draw a Trends chart as the Trends view draws it";
    render.spec.description =
        "The chart is drawn by the GUI's own Trends plot, in the GUI's colours, with\n"
        "its curves, axes, legend, and season and event markers. With no dates, it\n"
        "covers all dates, as the All Dates season: from the first activity to the\n"
        "last, planned ones included.\n"
        "--by changes the grouping for this drawing only.";
    render.spec.scope = Scope::Athlete;
    chartParams(render.spec);
    render.spec.params << imageParams(false);
    render.spec.httpMethod = "GET";
    render.spec.httpPath = "/athletes/{athlete}/charts/{name}/image";
    render.handler = renderChart;
    registry.add(render);

    Command data;
    data.spec.name = "chart.library.data";
    data.spec.summary = "the data of a Trends chart, as its Data Table and Export Chart Data";
    data.spec.description =
        "One row per day, week, month or year the chart groups by, and a column per\n"
        "curve, computed as the chart computes its curves. Grouped by day, days with\n"
        "no values are left out, as the GUI's table does. The dates, --by and\n"
        "--filter are those of 'chart library render'.";
    data.spec.scope = Scope::Athlete;
    chartParams(data.spec);
    data.spec.httpMethod = "GET";
    data.spec.httpPath = "/athletes/{athlete}/charts/{name}/data";
    data.handler = chartData;
    registry.add(data);
}

} // namespace Headless
