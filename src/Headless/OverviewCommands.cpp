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
// The activity overview as the GUI shows it: the overview charts of the
// athlete's analysis layout that the GUI picks for the activity, every tile
// evaluated the way the tile itself does, with values formatted as shown.
//

#include "HeadlessCommands.h"
#include "ProgramArgs.h"
#include "ActivitySelection.h"
#include "ActivityJson.h"
#include "IntervalData.h"
#include "MetricData.h"
#include "ZoneData.h"
#include "ResultFormat.h"

#include "Context.h"
#include "Athlete.h"
#include "RideItem.h"
#include "RideFile.h"
#include "IntervalItem.h"
#include "RideMetric.h"
#include "DataFilter.h"
#include "Specification.h"
#include "PMCData.h"
#include "TimeUtils.h"
#include "Utils.h"
#include "Overview.h"
#include "GcWindowRegistry.h"
#include "PerspectiveConfigParser.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <cmath>

namespace Headless {

// the overview windows as the GUI stores them; the tiles are OverviewItemType
static const int overviewWindow = GcWindowTypes::Overview, blankOverviewWindow = GcWindowTypes::OverviewAnalysisBlank;

struct OverviewChart {
    QString title;
    QJsonArray tiles;           // the tiles' saved configuration
};

struct OverviewLayout {
    QString name, expression;
    QList<OverviewChart> charts;
};

// the overview a new athlete starts with (read once)
static QJsonArray
defaultTiles()
{
    static const QJsonArray tiles = QJsonDocument::fromJson(OverviewWindow::defaultConfig(OverviewScope::ANALYSIS).toUtf8())
                                    .object()["CHARTS"].toArray();
    return tiles;
}

// the analysis layouts, as AbstractView::restoreState reads them
static QList<OverviewLayout>
readLayouts(Athlete *athlete, QString &source)
{
    source = athlete->home->config().absoluteFilePath("analysis-perspectives.xml");
    if (!QFileInfo::exists(source)) source = ":xml/analysis-perspectives.xml";

    QList<OverviewLayout> layouts;
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly)) return layouts;

    QXmlInputSource input(&file);
    QXmlSimpleReader reader;
    PerspectiveConfigParser parser(VIEW_ANALYSIS);
    reader.setContentHandler(&parser);
    reader.setErrorHandler(&parser);
    reader.parse(input);

    for (const PerspectiveConfig &p : parser.layouts) {
        OverviewLayout l;
        l.name = p.name;
        l.expression = p.expression;
        for (const PerspectiveChartConfig &chart : p.charts) {
            if (chart.id != overviewWindow && chart.id != blankOverviewWindow) continue;
            OverviewChart c;
            c.title = chart.title;
            // an unconfigured overview gets the default tiles
            if (chart.id == overviewWindow) c.tiles = defaultTiles();
            const PerspectiveChartConfig::Property *config = chart.property("config");
            if (config) {
                QJsonObject root = QJsonDocument::fromJson(config->value.toUtf8()).object();
                if (root["version"].toString() == "2.0") c.tiles = root["CHARTS"].toArray();
            }
            l.charts << c;
        }
        layouts << l;
    }
    return layouts;
}

// as AnalysisView::findRidesPerspective: the first layout whose expression
// matches the activity, else the first one
static int
layoutFor(Context *context, const QList<OverviewLayout> &layouts, RideItem *item)
{
    for (int i = 0; i < layouts.count(); i++) {
        if (layouts[i].expression.trimmed().isEmpty()) continue;
        DataFilter df(nullptr, context, layouts[i].expression);
        if (df.getErrors().isEmpty() && df.evaluate(item, nullptr).number()) return i;
    }
    return 0;
}

//
// Tiles
//

// the GUI keeps a table sorted by the column last clicked (DataOverviewItem::sort)
static void
sortTable(const QVector<QString> &names, QVector<QString> &values, int column, bool ascending)
{
    if (names.isEmpty() || column < 0 || column >= names.count()) return;
    int rows = values.count() / names.count();
    if (rows < 2) return;

    // as DataOverviewItem::sort orders it
    QVector<QString> in;
    for (int i = rows * column; i < values.count() && i < rows * (column + 1); i++) in << values[i];
    QVector<int> order = Utils::argsortShown(in, ascending);

    QVector<QString> sorted = values;
    for (int c = 0; c < names.count(); c++)
        for (int k = 0; k < order.count() && c * rows + k < sorted.count(); k++)
            sorted[c * rows + k] = values[c * rows + order[k]];
    values = sorted;
}

// one tile as the overview shows it, and what the outputs are made of: the
// JSON, the lines of the text report, and the CSV lines (row, column,
// units, value). Each kind fills all of them in one place.
struct TileValue {
    QJsonObject json;
    QList<QStringList> lines;       // text, a line of cells each
    QList<QStringList> csv;         // row, column, units, value
    QStringList gridHead;           // a table as columns, for a CSV of just that table
    QList<QStringList> grid;

    void csvLine(const QString &row, const QString &column, const QString &units, const QString &value) {
        csv << QStringList{ row, column, units, value };
    }
};

// a table tile: a program with names, units and values functions (DataOverviewItem::setData)
static void
tableTile(Context *context, RideItem *item, const QJsonObject &config, TileValue &tile)
{
    DataFilter parser(nullptr, context, Utils::jsonunprotect2(config["program"].toString()));
    if (!parser.root() || !parser.errorList().isEmpty()) {
        tile.json.insert("error", parser.errorList().join("; "));
        return;
    }
    Specification spec;
    DateRange dr;
    auto strings = [&](const char *name) {
        Leaf *f = parser.rt.functions.value(name, nullptr);
        if (!f) return QVector<QString>();
        return parser.root()->eval(&parser.rt, f, Result(0), 0, item, nullptr, nullptr, spec, dr).asString();
    };
    QVector<QString> names = strings("names"), units = strings("units"), values = strings("values");
    for (QString &u : units) if (u == QObject::tr("seconds")) u.clear();   // shown as times

    // as the GUI: one column per name when there is more than one row of
    // values, else a list of name, value and units. Text and JSON keep that
    // list. CSV always uses the column grid, for no rows, one, or many.
    bool grid = !names.isEmpty() && values.count() > names.count();
    tile.json.insert("style", grid ? "grid" : "list");
    if (grid && config.contains("sortcolumn"))
        sortTable(names, values, config["sortcolumn"].toInt(-1), config["sortorder"].toInt() == Qt::AscendingOrder);

    // the column grid: one column per name, values column by column
    int records = names.isEmpty() ? 0 : values.count() / names.count();
    QJsonArray gridColumns, gridRows;
    bool anyUnits = false;
    for (int c = 0; c < names.count(); c++) {
        QString u = c < units.count() ? units[c] : QString();
        gridColumns.append(QJsonObject{ { "name", names[c] }, { "units", u } });
        tile.gridHead << (u.isEmpty() ? names[c] : QString("%1 (%2)").arg(names[c]).arg(u));
        if (!u.isEmpty()) anyUnits = true;
    }
    for (int r = 0; r < records; r++) {
        QJsonArray row;
        QStringList cells;
        for (int c = 0; c < names.count(); c++) {
            row.append(values[c * records + r]);
            cells << values[c * records + r];
        }
        gridRows.append(row);
        tile.grid << cells;
    }

    if (grid) {
        tile.json.insert("columns", gridColumns);
        tile.json.insert("rows", gridRows);
        QStringList head, unitLine;
        for (int c = 0; c < names.count(); c++) {
            head << names[c];
            unitLine << (c < units.count() ? units[c] : QString());
        }
        tile.lines << head;
        if (anyUnits) tile.lines << unitLine;
        for (int r = 0; r < tile.grid.count(); r++) {
            tile.lines << tile.grid[r];
            for (int c = 0; c < names.count(); c++) tile.csvLine(QString::number(r + 1), names[c], unitLine[c], tile.grid[r][c]);
        }
    } else {
        // name, value and units per line
        QJsonArray columns, rows;
        for (const char *c : { "name", "value", "units" }) columns.append(QJsonObject{ { "name", c }, { "units", "" } });
        for (int r = 0; r < names.count(); r++) {
            QStringList cells{ names[r], r < values.count() ? values[r] : QString(), r < units.count() ? units[r] : QString() };
            rows.append(QJsonArray::fromStringList(cells));
            tile.lines << cells;
            tile.csvLine(cells[0], "value", cells[2], cells[1]);
        }
        tile.json.insert("columns", columns);
        tile.json.insert("rows", rows);
    }
}

static void
zoneTile(AthleteSession &session, RideItem *item, const QJsonObject &config, TileValue &tile)
{
    int series = config["series"].toInt();
    bool polarized = config["polarized"].toInt() != 0;
    static const QMap<int, QString> types = { { RideFile::watts, "power" }, { RideFile::hr, "hr" },
                                              { RideFile::kph, "pace" }, { RideFile::wbal, "fatigue" } };
    QString type = types.value(series, "power");
    tile.json.insert("zones", type);

    // names and times as the tile's bars, percent of the time in all of them
    QStringList names;
    QVector<double> seconds;
    if (polarized && type != "fatigue") {
        QString prefix = type == "hr" ? "time_in_zone_H" : type == "pace" ? "time_in_zone_P" : "time_in_zone_L";
        for (const char *z : { "I", "II", "III" }) { names << z; seconds << std::round(item->getForSymbol(prefix + z)); }
    } else {
        ActivityZones zones;
        QString error;
        if (!activityZones(session.athlete(), item, type, zones, error)) { tile.json.insert("error", error); return; }
        for (const ZoneRow &r : zones.rows) { names << r.name; seconds << std::round(r.seconds); }
    }
    double sum = 0;
    for (double s : seconds) sum += s;
    QJsonArray rows;
    for (int i = 0; i < names.count(); i++) {
        QJsonObject z;
        QString time = time_to_string(seconds[i], true);
        double percent = sum > 0 ? std::round(seconds[i] / sum * 100) : 0;
        z.insert("name", names[i]);
        z.insert("time", time);
        z.insert("seconds", seconds[i]);
        z.insert("percent", percent);
        rows.append(z);
        tile.lines << QStringList{ names[i], time, QString("%1 %").arg(percent) };
        tile.csvLine(names[i], "time", "", time);
        tile.csvLine(names[i], "percent", "%", ResultFormat::csvValue(z["percent"]));
    }
    tile.json.insert("rows", rows);
}

static void
intervalTile(RideItem *item, const QJsonObject &config, bool metricUnits, TileValue &tile)
{
    // the bubble chart: one bubble per interval
    QJsonArray axes, rows;
    QStringList symbols = { config["xsymbol"].toString(), config["ysymbol"].toString(), config["zsymbol"].toString() };
    QStringList head{ "" }, axisNames, axisUnits;
    for (const QString &s : symbols) {
        const RideMetric *m = RideMetricFactory::instance().rideMetric(s);
        axisNames << (m ? m->name() : s);
        axisUnits << (m ? m->units(metricUnits) : QString());
        axes.append(QJsonObject{ { "symbol", s }, { "name", axisNames.last() }, { "units", axisUnits.last() } });
        head << axisNames.last();
    }
    tile.lines << head;
    for (IntervalItem *i : item->intervals()) {
        QJsonObject r;
        QStringList line{ i->name };
        r.insert("name", i->name);
        for (int a = 0; a < 3; a++) {
            QString v = i->getStringForSymbol(symbols[a], metricUnits);
            r.insert(QString("xyz").mid(a, 1), v);
            line << v;
            tile.csvLine(i->name, axisNames[a], axisUnits[a], v);
        }
        rows.append(r);
        tile.lines << line;
    }
    tile.json.insert("axes", axes);
    tile.json.insert("rows", rows);
}

// a tile showing one value: metric, field, RPE, KPI
static void
valueTile(TileValue &tile, const QString &value, const QString &units)
{
    tile.json.insert("value", value);
    tile.json.insert("units", units);
    tile.lines << QStringList{ value, units };
    tile.csvLine("", "value", units, value);
}

static void
noteTile(TileValue &tile, const QString &note)
{
    tile.json.insert("note", note);
    tile.lines << QStringList{ note };
    tile.csvLine("", "note", "", note);
}

static TileValue
evaluateTile(AthleteSession &session, RideItem *item, const QJsonObject &config, bool metricUnits)
{
    Context *context = session.context();
    int type = config["type"].toInt();
    TileValue tile;
    tile.json.insert("name", config["name"].toString());

    switch (type) {
    case DATATABLE:
        tile.json.insert("kind", "table");
        tableTile(context, item, config, tile);
        break;

    case METRIC: {
        QString symbol = config["symbol"].toString();
        const RideMetric *m = RideMetricFactory::instance().rideMetric(symbol);
        QString value = item->getStringForSymbol(symbol, metricUnits);
        tile.json.insert("kind", "metric");
        tile.json.insert("symbol", symbol);
        QString units = m ? m->units(metricUnits) : QString();
        valueTile(tile, value == "nan" ? QString() : value, units == QObject::tr("seconds") ? QString() : units);
        break;
    }

    case META:
        tile.json.insert("kind", "field");
        tile.json.insert("field", config["symbol"].toString());
        tile.json.insert("value", item->getText(config["symbol"].toString(), ""));
        tile.lines << QStringList{ tile.json["value"].toString(), QString() };
        tile.csvLine("", "value", "", tile.json["value"].toString());
        break;

    case RPE:
        tile.json.insert("kind", "rpe");
        tile.json.insert("value", item->getText("RPE", "0"));
        tile.lines << QStringList{ tile.json["value"].toString(), QString() };
        tile.csvLine("", "value", "", tile.json["value"].toString());
        break;

    case KPI: {
        DataFilter parser(nullptr, context, Utils::jsonunprotect2(config["program"].toString()));
        QString value = parser.evaluate(item, nullptr).string();
        if (value == "nan") value.clear();
        if (config["istime"].toInt()) value = time_to_string(value.toDouble(), true);
        tile.json.insert("kind", "kpi");
        valueTile(tile, value, config["units"].toString());
        if (!parser.getErrors().isEmpty()) tile.json.insert("error", parser.getErrors().join("; "));
        break;
    }

    case ZONE:
        tile.json.insert("kind", "zones");
        zoneTile(session, item, config, tile);
        break;

    case PMC: {
        // shown as whole numbers, without a title (PMCOverviewItem)
        QString symbol = config["symbol"].toString();
        PMCData *pmc = pmcFor(session, symbol);
        QDate day = item->dateTime.date();
        tile.json.insert("kind", "pmc");
        tile.json.insert("metric", symbol);
        if (pmc) {
            tile.json.insert("form", std::round(pmc->sb(day)));
            tile.json.insert("fitness", std::round(pmc->lts(day)));
            tile.json.insert("fatigue", std::round(pmc->sts(day)));
            tile.json.insert("risk", std::round(pmc->rr(day)));
        }
        for (const char *k : { "form", "fitness", "fatigue", "risk" }) {
            tile.lines << QStringList{ QString(k).replace(0, 1, QString(k).at(0).toUpper()), QString::number(tile.json[k].toDouble()) };
            tile.csvLine("", k, "", ResultFormat::csvValue(tile.json[k]));
        }
        break;
    }

    case INTERVAL:
        tile.json.insert("kind", "intervals");
        intervalTile(item, config, metricUnits, tile);
        break;

    case ROUTE:
        tile.json.insert("kind", "route");
        noteTile(tile, "a map, export the track with 'activity export --as gpx'");
        break;

    default:
        tile.json.insert("kind", "chart");
        noteTile(tile, "a chart, not reproduced as data");
        break;
    }
    return tile;
}

//
// Text: each tile as the GUI lays it out
//

static QString
columnsText(const QList<QStringList> &lines)
{
    QVector<int> widths;
    for (const QStringList &l : lines)
        for (int c = 0; c < l.count(); c++) {
            if (widths.count() <= c) widths << 0;
            widths[c] = std::max(widths[c], int(l[c].length()));
        }
    QString text;
    for (const QStringList &l : lines) {
        QString line;
        for (int c = 0; c < l.count(); c++) line += (c ? "  " : "") + l[c].leftJustified(widths[c]);
        // blank leading cells keep the columns aligned, only trailing ones go
        while (line.endsWith(' ')) line.chop(1);
        text += "  " + line + "\n";
    }
    return text;
}

static QString
tileText(const TileValue &tile)
{
    QString text = tile.json["name"].toString();
    QString kind = tile.json["kind"].toString();
    if (text.isEmpty()) text = kind == "pmc" ? QString("PMC") : kind;
    text += "\n";
    if (tile.json.contains("error")) return text + "  error: " + tile.json["error"].toString() + "\n";
    return text + columnsText(tile.lines);
}

//
// CSV: one table tile as that table's columns, else a line per value.
// The interval count line stays in the text report and in JSON.
//

// a data table whose program lists intervals, as the default Intervals tiles do
static bool
intervalProgram(const QString &program)
{
    return program.contains(QLatin1String("intervalstrings(")) || program.contains(QLatin1String("intervals("));
}

static QString
tilesCsv(const QList<TileValue> &tiles)
{
    auto line = [](const QStringList &f) { return ResultFormat::csvLine(f); };

    if (tiles.count() == 1 && tiles[0].json["kind"] == "table" && !tiles[0].json.contains("error")) {
        QString text = line(tiles[0].gridHead);
        for (const QStringList &r : tiles[0].grid) text += line(r);
        return text;
    }

    QString text = line({ "tile", "kind", "row", "column", "units", "value" });
    for (const TileValue &t : tiles) {
        QString name = t.json["name"].toString(), kind = t.json["kind"].toString();
        if (t.json.contains("error")) {
            text += line({ name, kind, "", "error", "", t.json["error"].toString() });
            continue;
        }
        for (const QStringList &c : t.csv) text += line(QStringList{ name, kind } + c);
    }
    return text;
}

static CommandResult
overviewCommand(CommandEnvironment &env, const CommandRequest &request)
{
    QString error;
    RideItem *item = env.session->findActivity(request.args.value("activity").toString(), error);
    if (!item) return CommandResult::failure(Status::NotFound, error);

    QString source;
    QList<OverviewLayout> layouts = readLayouts(env.session->athlete(), source);
    if (layouts.isEmpty()) return CommandResult::failure(Status::Failed, QString("no analysis layouts in %1").arg(source));

    int chosen = layoutFor(env.session->context(), layouts, item);
    QString wanted = request.args.value("layout").toString();
    if (!wanted.isEmpty()) {
        QStringList names;
        chosen = -1;
        for (int i = 0; i < layouts.count(); i++) {
            names << layouts[i].name;
            if (layouts[i].name.compare(wanted, Qt::CaseInsensitive) == 0) chosen = i;
        }
        if (chosen < 0) return CommandResult::failure(Status::NotFound,
                                QString("no layout '%1', the layouts are: %2").arg(wanted).arg(names.join(", ")));
    }
    const OverviewLayout &layout = layouts[chosen];

    // formulas use the GUI's units setting, so the tiles do too
    bool metricUnits = GlobalContext::context()->useMetricUnits;
    QStringList tileNames = splitList(request.args.value("tile"));
    IntervalCensus census = intervalCensus(item);
    QString censusLine = intervalCensusLine(census);

    // some formula functions look at the activity the GUI has selected
    Context *context = env.session->context();
    RideItem *selected = context->ride;
    context->ride = item;

    QJsonArray tiles;
    QList<TileValue> values;
    QString text;
    bool showedCensus = false;
    for (const OverviewChart &chart : layout.charts) {
        // columns left to right, top to bottom within a column
        QList<QJsonObject> configs;
        for (const QJsonValue &v : chart.tiles) configs << v.toObject();
        std::stable_sort(configs.begin(), configs.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return a["column"].toInt() != b["column"].toInt() ? a["column"].toInt() < b["column"].toInt()
                                                              : a["order"].toInt() < b["order"].toInt();
        });
        for (const QJsonObject &config : configs) {
            if (!tileNames.isEmpty()) {
                bool match = false;
                for (const QString &n : tileNames) if (config["name"].toString().trimmed().compare(n, Qt::CaseInsensitive) == 0) match = true;
                if (!match) continue;
            }
            TileValue tile = evaluateTile(*env.session, item, config, metricUnits);
            tile.json.insert("chart", chart.title);
            tile.json.insert("column", config["column"].toInt());
            int tileType = config["type"].toInt();
            bool isIntervals = tileType == INTERVAL
                || (tileType == DATATABLE && intervalProgram(config["program"].toString()));
            tiles.append(tile.json);
            values << tile;
            if (isIntervals && !showedCensus) {
                text += censusLine + "\n";
                showedCensus = true;
            }
            text += tileText(tile) + "\n";
        }
    }
    context->ride = selected;

    if (!tileNames.isEmpty() && tiles.isEmpty())
        return CommandResult::failure(Status::NotFound,
                    QString("no tile called '%1' in the '%2' layout").arg(tileNames.join("', '")).arg(layout.name));

    QString csv = tilesCsv(values);

    QJsonObject data;
    data.insert("activity", QFileInfo(item->fileName).completeBaseName());
    data.insert("layout", layout.name);
    data.insert("recorded_laps", census.recordedLaps);
    data.insert("user_intervals", census.userIntervals);
    data.insert("discovered_efforts", census.discoveredEfforts);
    data.insert("tiles", tiles);
    CommandResult result = CommandResult::success(data);
    result.csv = csv;
    result.text = QString("%1, %2 layout\n\n").arg(activityStart(item).replace("T", " ")).arg(layout.name) + text;
    return result;
}

//
// Layout tiles
//
// The intervals table on the activity overview is not the favourites list.
// It is the program on a table tile (Intervals Data on the Run and Swim
// layouts), stored in config/analysis-perspectives.xml. These commands
// show that program and replace it, the same edit as Tile Settings.
//

static QString
tileKind(int type)
{
    switch (type) {
    case RPE: return "rpe";
    case METRIC: return "metric";
    case META: return "field";
    case ZONE: return "zones";
    case INTERVAL: return "intervals";
    case PMC: return "pmc";
    case ROUTE: return "route";
    case KPI: return "kpi";
    case DATATABLE: return "table";
    default: return "chart";
    }
}

static QString
perspectivesPath(Athlete *athlete)
{
    return athlete->home->config().absoluteFilePath("analysis-perspectives.xml");
}

struct TileHit {
    int chart;          // which overview chart in the layout
    int index;          // which tile in that chart's CHARTS array
    QJsonObject config;
};

static QList<TileHit>
findTiles(const OverviewLayout &layout, const QString &name)
{
    QList<TileHit> hits;
    for (int c = 0; c < layout.charts.count(); c++) {
        const QJsonArray &tiles = layout.charts.at(c).tiles;
        for (int i = 0; i < tiles.count(); i++) {
            QJsonObject tile = tiles.at(i).toObject();
            if (tile["name"].toString().trimmed().compare(name, Qt::CaseInsensitive) == 0)
                hits << TileHit{ c, i, tile };
        }
    }
    return hits;
}

static QStringList
tileNames(const OverviewLayout &layout)
{
    QStringList names;
    for (const OverviewChart &chart : layout.charts) {
        for (const QJsonValue &v : chart.tiles) {
            QString name = v.toObject()["name"].toString().trimmed();
            if (!name.isEmpty() && !names.contains(name)) names << name;
        }
    }
    return names;
}

static CommandResult
resolveLayout(const QList<OverviewLayout> &layouts, const QString &wanted, int &index)
{
    QStringList names;
    for (const OverviewLayout &l : layouts) names << l.name;
    if (wanted.isEmpty()) {
        if (layouts.count() != 1)
            return CommandResult::failure(Status::Usage,
                QString("more than one layout, choose one with --layout (%1)").arg(names.join(", ")));
        index = 0;
        return CommandResult::success();
    }
    index = -1;
    for (int i = 0; i < layouts.count(); i++)
        if (layouts.at(i).name.compare(wanted, Qt::CaseInsensitive) == 0) index = i;
    if (index < 0)
        return CommandResult::failure(Status::NotFound,
            QString("no layout '%1', the layouts are: %2").arg(wanted).arg(names.join(", ")));
    return CommandResult::success();
}

static CommandResult
openLayouts(Athlete *athlete, QList<OverviewLayout> &layouts, QString &source)
{
    layouts = readLayouts(athlete, source);
    if (layouts.isEmpty())
        return CommandResult::failure(Status::Failed, QString("no analysis layouts in %1").arg(source));
    return CommandResult::success();
}

static CommandResult
listLayouts(CommandEnvironment &env, const CommandRequest &)
{
    QString source;
    QList<OverviewLayout> layouts;
    CommandResult opened = openLayouts(env.session->athlete(), layouts, source);
    if (!opened.ok()) return opened;

    QJsonArray list;
    for (const OverviewLayout &l : layouts) {
        QJsonObject o;
        o.insert("name", l.name);
        o.insert("expression", l.expression);
        list.append(o);
    }
    QJsonObject data;
    data.insert("layouts", list);
    data.insert("file", source);
    return CommandResult::success(data);
}

static CommandResult
listTiles(CommandEnvironment &env, const CommandRequest &request)
{
    QString source;
    QList<OverviewLayout> layouts;
    CommandResult opened = openLayouts(env.session->athlete(), layouts, source);
    if (!opened.ok()) return opened;

    int index = 0;
    CommandResult chosen = resolveLayout(layouts, request.args.value("layout").toString(), index);
    if (!chosen.ok()) return chosen;
    const OverviewLayout &layout = layouts.at(index);

    QJsonArray tiles;
    for (const OverviewChart &chart : layout.charts) {
        for (const QJsonValue &v : chart.tiles) {
            QJsonObject config = v.toObject();
            QJsonObject o;
            o.insert("name", config["name"].toString());
            o.insert("kind", tileKind(config["type"].toInt()));
            o.insert("chart", chart.title);
            tiles.append(o);
        }
    }
    QJsonObject data;
    data.insert("layout", layout.name);
    data.insert("expression", layout.expression);
    data.insert("tiles", tiles);
    return CommandResult::success(data);
}

static CommandResult
locateTile(Athlete *athlete, const CommandRequest &request, OverviewLayout &layout, TileHit &hit, QString &source)
{
    QList<OverviewLayout> layouts;
    CommandResult opened = openLayouts(athlete, layouts, source);
    if (!opened.ok()) return opened;

    int index = 0;
    CommandResult chosen = resolveLayout(layouts, request.args.value("layout").toString(), index);
    if (!chosen.ok()) return chosen;
    layout = layouts.at(index);

    QString name = request.args.value("tile").toString();
    QList<TileHit> hits = findTiles(layout, name);
    if (hits.isEmpty())
        return CommandResult::failure(Status::NotFound,
            QString("no tile called '%1' in the '%2' layout, the tiles are: %3")
                .arg(name).arg(layout.name).arg(tileNames(layout).join(", ")));
    if (hits.count() > 1)
        return CommandResult::failure(Status::Usage,
            QString("more than one tile called '%1' in the '%2' layout").arg(name).arg(layout.name));
    hit = hits.first();
    return CommandResult::success();
}

static QString
tileProgram(const QJsonObject &config)
{
    return Utils::jsonunprotect2(config["program"].toString());
}

static CommandResult
showTile(CommandEnvironment &env, const CommandRequest &request)
{
    OverviewLayout layout;
    TileHit hit;
    QString source;
    CommandResult found = locateTile(env.session->athlete(), request, layout, hit, source);
    if (!found.ok()) return found;
    if (!hit.config.contains("program"))
        return CommandResult::failure(Status::Usage, QString("tile '%1' has no program").arg(hit.config["name"].toString()));

    QString program = tileProgram(hit.config);
    QJsonObject data;
    data.insert("layout", layout.name);
    data.insert("name", hit.config["name"].toString());
    data.insert("kind", tileKind(hit.config["type"].toInt()));
    data.insert("program", program);
    data.insert("file", source);
    CommandResult result = CommandResult::success(data);
    result.text = program.endsWith('\n') ? program : program + "\n";
    return result;
}

// replace one tile's program inside an overview chart's config attribute
static bool
patchConfig(const QString &attributeValue, int tileIndex, const QString &program, QString &patched, QString &error)
{
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(Utils::unprotect(attributeValue).toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        error = "overview config is not JSON";
        return false;
    }
    QJsonObject root = doc.object();
    QJsonArray charts = root["CHARTS"].toArray();
    if (tileIndex < 0 || tileIndex >= charts.count()) {
        error = "tile is not in this overview";
        return false;
    }
    QJsonObject tile = charts.at(tileIndex).toObject();
    if (!tile.contains("program")) {
        error = QString("tile '%1' has no program").arg(tile["name"].toString());
        return false;
    }
    tile.insert("program", Utils::jsonprotect2(program));
    charts.replace(tileIndex, tile);
    root.insert("CHARTS", charts);
    patched = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return true;
}

static CommandResult
ensurePerspectives(Athlete *athlete, QString &path)
{
    path = perspectivesPath(athlete);
    if (QFileInfo::exists(path)) return CommandResult::success();

    QFile in(":xml/analysis-perspectives.xml");
    if (!in.open(QIODevice::ReadOnly))
        return CommandResult::failure(Status::Failed, "can't read the default analysis layouts");
    if (!athlete->home->config().mkpath("."))
        return CommandResult::failure(Status::Failed, QString("can't create %1").arg(athlete->home->config().absolutePath()));

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return CommandResult::failure(Status::Failed, QString("can't write %1").arg(path));
    if (out.write(in.readAll()) < 0)
        return CommandResult::failure(Status::Failed, QString("can't write %1").arg(path));
    return CommandResult::success();
}

static CommandResult
writeTileProgram(const QString &path, const QString &layoutName, const TileHit &hit, const QString &program)
{
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly))
        return CommandResult::failure(Status::Failed, QString("can't read %1").arg(path));
    QByteArray original = in.readAll();
    in.close();

    QXmlStreamReader xml(original);
    QByteArray rewritten;
    QXmlStreamWriter out(&rewritten);
    out.setAutoFormatting(false);

    bool inLayout = false;
    int window = -1;
    int overviewCharts = -1;
    bool patched = false;
    QString error;

    while (!xml.atEnd()) {
        xml.readNext();
        switch (xml.tokenType()) {
        case QXmlStreamReader::StartDocument:
            out.writeStartDocument();
            break;
        case QXmlStreamReader::EndDocument:
            out.writeEndDocument();
            break;
        case QXmlStreamReader::StartElement: {
            out.writeStartElement(xml.qualifiedName().toString());
            QXmlStreamAttributes attrs = xml.attributes();
            if (xml.name() == QLatin1String("layout")) {
                inLayout = Utils::unprotect(attrs.value("name").toString()).compare(layoutName, Qt::CaseInsensitive) == 0;
                window = -1;
                overviewCharts = -1;
            } else if (xml.name() == QLatin1String("chart")) {
                window = attrs.value("id").toInt();
                if (inLayout && (window == overviewWindow || window == blankOverviewWindow)) overviewCharts++;
            }
            bool configProp = xml.name() == QLatin1String("property")
                && inLayout && overviewCharts == hit.chart
                && (window == overviewWindow || window == blankOverviewWindow)
                && attrs.value("name") == QLatin1String("config");
            for (const QXmlStreamAttribute &a : attrs) {
                QString value = a.value().toString();
                if (configProp && a.name() == QLatin1String("value")) {
                    if (!patchConfig(value, hit.index, program, value, error))
                        return CommandResult::failure(Status::Failed, error);
                    patched = true;
                }
                out.writeAttribute(a.qualifiedName().toString(), value);
            }
            break;
        }
        case QXmlStreamReader::EndElement:
            out.writeEndElement();
            break;
        case QXmlStreamReader::Characters:
            out.writeCharacters(xml.text().toString());
            break;
        case QXmlStreamReader::Comment:
            out.writeComment(xml.text().toString());
            break;
        case QXmlStreamReader::EntityReference:
            out.writeEntityReference(xml.name().toString());
            break;
        case QXmlStreamReader::ProcessingInstruction:
            out.writeProcessingInstruction(xml.processingInstructionTarget().toString(),
                                           xml.processingInstructionData().toString());
            break;
        default:
            break;
        }
    }
    if (xml.hasError())
        return CommandResult::failure(Status::Failed, QString("can't read %1: %2").arg(path).arg(xml.errorString()));
    if (!patched)
        return CommandResult::failure(Status::Failed, QString("didn't find the tile in %1").arg(path));

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return CommandResult::failure(Status::Failed, QString("can't write %1").arg(path));
    if (file.write(rewritten) < 0 || !file.commit())
        return CommandResult::failure(Status::Failed, QString("can't write %1").arg(path));
    return CommandResult::success();
}

static CommandResult
setTile(CommandEnvironment &env, const CommandRequest &request)
{
    OverviewLayout layout;
    TileHit hit;
    QString source;
    CommandResult found = locateTile(env.session->athlete(), request, layout, hit, source);
    if (!found.ok()) return found;
    if (!hit.config.contains("program"))
        return CommandResult::failure(Status::Usage, QString("tile '%1' has no program").arg(hit.config["name"].toString()));

    // a tile program uses names, units and values, no value block
    QString program;
    CommandResult read = readProgramArg(request, true, program);
    if (!read.ok()) return read;
    CommandResult compiled = checkProgram(env.session->context(), program, false);
    if (!compiled.ok()) return compiled;

    QString path;
    CommandResult ready = ensurePerspectives(env.session->athlete(), path);
    if (!ready.ok()) return ready;
    CommandResult written = writeTileProgram(path, layout.name, hit, program);
    if (!written.ok()) return written;

    QJsonObject data;
    data.insert("status", "updated");
    data.insert("layout", layout.name);
    data.insert("name", hit.config["name"].toString());
    data.insert("kind", tileKind(hit.config["type"].toInt()));
    data.insert("program", program);
    data.insert("file", path);
    CommandResult result = CommandResult::success(data);
    result.text = QString("updated %1 / %2\n").arg(layout.name).arg(hit.config["name"].toString());
    return result;
}

void
registerOverviewCommands(CommandRegistry &registry)
{
    Command overview;
    overview.spec.name = "activity.overview";
    overview.spec.summary = "the activity overview as the GUI shows it: its tables and tiles, formatted the same";
    overview.spec.description = "Uses the athlete's analysis layout that the GUI switches to for the activity "
                                "(the first whose expression matches, e.g. isRun, else the first one).";
    overview.spec.scope = Scope::Athlete;
    overview.spec.params << ParamSpec("activity", ParamType::String, "activity id, start time, date, 'first' or 'last'").req().pos();
    overview.spec.params << ParamSpec("tile", ParamType::String, "only the tiles with this title, e.g. Intervals").many();
    overview.spec.params << ParamSpec("layout", ParamType::String, "use this layout instead of the one the GUI picks");
    overview.spec.httpMethod = "GET";
    overview.spec.httpPath = "/athletes/{athlete}/activities/{activity}/overview";
    overview.handler = overviewCommand;
    registry.add(overview);

    Command layouts;
    layouts.spec.name = "layout.list";
    layouts.spec.summary = "list the analysis layouts the activity overview switches between";
    layouts.spec.scope = Scope::Athlete;
    layouts.spec.httpMethod = "GET";
    layouts.spec.httpPath = "/athletes/{athlete}/layouts";
    layouts.handler = listLayouts;
    registry.add(layouts);

    Command tiles;
    tiles.spec.name = "layout.tile.list";
    tiles.spec.summary = "list the tiles on an analysis layout";
    tiles.spec.scope = Scope::Athlete;
    tiles.spec.params << ParamSpec("layout", ParamType::String, "layout name, e.g. Run; required when there is more than one");
    tiles.spec.httpMethod = "GET";
    tiles.spec.httpPath = "/athletes/{athlete}/layouts/{layout}/tiles";
    tiles.handler = listTiles;
    registry.add(tiles);

    Command show;
    show.spec.name = "layout.tile.show";
    show.spec.summary = "show the program of an overview tile, as Tile Settings does";
    show.spec.description =
        "The intervals table on the activity overview is this program, not the\n"
        "favourites list. On the Run and Swim layouts the tile is 'Intervals Data'.";
    show.spec.scope = Scope::Athlete;
    show.spec.params << ParamSpec("tile", ParamType::String, "tile title, e.g. Intervals Data").req().pos();
    show.spec.params << ParamSpec("layout", ParamType::String, "layout name, e.g. Run; required when there is more than one");
    show.spec.httpMethod = "GET";
    show.spec.httpPath = "/athletes/{athlete}/layouts/{layout}/tiles/{tile}";
    show.handler = showTile;
    registry.add(show);

    Command set;
    set.spec.name = "layout.tile.set";
    set.spec.summary = "replace the program of an overview tile";
    set.spec.description =
        "A column is a formula name, as 'metric list' prints it (Average_Heart_Rate).\n"
        "A name that is not one symbol, such as HRR/v, is refused and the file is\n"
        "left unchanged. The program is checked before it is written.";
    set.spec.scope = Scope::Athlete;
    set.spec.modifies = true;
    set.spec.params << ParamSpec("tile", ParamType::String, "tile title, e.g. Intervals Data").req().pos();
    set.spec.params << ParamSpec("layout", ParamType::String, "layout name, e.g. Run; required when there is more than one");
    set.spec.params << ParamSpec("program", ParamType::String, "the tile program");
    set.spec.params << programFileParam("read the program from this file, or - for stdin");
    set.spec.httpMethod = "PUT";
    set.spec.httpPath = "/athletes/{athlete}/layouts/{layout}/tiles/{tile}";
    set.handler = setTile;
    registry.add(set);
}

} // namespace Headless
