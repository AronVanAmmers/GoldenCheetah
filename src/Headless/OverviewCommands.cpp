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
#include "ActivitySelection.h"
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

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTextStream>
#include <QXmlStreamReader>
#include <cmath>

namespace Headless {

// window and tile ids as the GUI stores them (GcWindowRegistry.h, OverviewItems.h)
static const int overviewWindow = 42, blankOverviewWindow = 49;
enum TileType { RPE = 100, METRIC, META, ZONE, INTERVAL, PMC, ROUTE, KPI,
                TOPN, DONUT, ACTIVITIES, ATHLETE, DATATABLE, USERCHART };

struct OverviewChart {
    QString title;
    QJsonArray tiles;           // the tiles' saved configuration
};

struct OverviewLayout {
    QString name, expression;
    QList<OverviewChart> charts;
};

// the overview a new athlete starts with
static QJsonArray
defaultTiles()
{
    QFile file(":charts/overview-analysis.gchart");
    if (!file.open(QIODevice::ReadOnly)) return QJsonArray();
    QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    QString config = root["CHART"].toObject()["PROPERTIES"].toObject()["config"].toString();
    return QJsonDocument::fromJson(config.toUtf8()).object()["CHARTS"].toArray();
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

    QXmlStreamReader xml(&file);
    int window = -1;
    while (!xml.atEnd()) {
        if (!xml.readNextStartElement()) continue;
        QXmlStreamAttributes a = xml.attributes();
        if (xml.name() == QLatin1String("layout")) {
            OverviewLayout l;
            l.name = Utils::unprotect(a.value("name").toString());
            l.expression = Utils::unprotect(a.value("expression").toString());
            layouts << l;
            window = -1;
        } else if (xml.name() == QLatin1String("chart") && !layouts.isEmpty()) {
            window = a.value("id").toInt();
            if (window == overviewWindow || window == blankOverviewWindow) {
                OverviewChart c;
                c.title = Utils::unprotect(a.value("title").toString());
                // an unconfigured overview gets the default tiles
                if (window == overviewWindow) c.tiles = defaultTiles();
                layouts.last().charts << c;
            }
        } else if (xml.name() == QLatin1String("property") && (window == overviewWindow || window == blankOverviewWindow)
                   && a.value("name") == QLatin1String("config")) {
            QString config = Utils::unprotect(a.value("value").toString());
            QJsonObject root = QJsonDocument::fromJson(config.toUtf8()).object();
            if (root["version"].toString() == "2.0") layouts.last().charts.last().tiles = root["CHARTS"].toArray();
        }
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

    static const QRegularExpression renumber("^[0-9.-]*$"), retime("^[0-9:]*$");
    bool strings = false;
    for (int i = rows * column; i < values.count() && i < rows * (column + 1); i++)
        if (!renumber.match(values[i]).hasMatch() && !retime.match(values[i]).hasMatch()) strings = true;

    QVector<int> order;
    if (strings) {
        QVector<QString> in;
        for (int i = rows * column; i < rows * (column + 1); i++) in << values[i];
        order = Utils::argsort(in, ascending);
    } else {
        QVector<double> in;
        for (int i = rows * column; i < rows * (column + 1); i++) {
            const QString &v = values[i];
            if (renumber.match(v).hasMatch()) { in << v.toDouble(); continue; }
            QTime t;
            for (const char *f : { "h:mm:ss", "hh:mm:ss", "mm:ss", "s" }) if ((t = QTime::fromString(v, f)).isValid()) break;
            in << QTime(0, 0, 0).secsTo(t);
        }
        order = Utils::argsort(in, ascending);
    }

    QVector<QString> sorted = values;
    for (int c = 0; c < names.count(); c++)
        for (int k = 0; k < order.count() && c * rows + k < sorted.count(); k++)
            sorted[c * rows + k] = values[c * rows + order[k]];
    values = sorted;
}

// a table tile: a program with names, units and values functions (DataOverviewItem::setData)
static void
tableTile(Context *context, RideItem *item, const QJsonObject &config, QJsonObject &tile)
{
    DataFilter parser(nullptr, context, Utils::jsonunprotect2(config["program"].toString()));
    if (!parser.root() || !parser.errorList().isEmpty()) {
        tile.insert("error", parser.errorList().join("; "));
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
    // values, else a list of name, value and units
    QJsonArray columns, rows;
    bool grid = !names.isEmpty() && values.count() > names.count();
    tile.insert("style", grid ? "grid" : "list");
    if (grid) {
        // one column per name, values column by column
        if (config.contains("sortcolumn"))
            sortTable(names, values, config["sortcolumn"].toInt(-1), config["sortorder"].toInt() == Qt::AscendingOrder);
        int n = values.count() / names.count();
        for (int c = 0; c < names.count(); c++) {
            QJsonObject col;
            col.insert("name", names[c]);
            col.insert("units", c < units.count() ? units[c] : QString());
            columns.append(col);
        }
        for (int r = 0; r < n; r++) {
            QJsonArray row;
            for (int c = 0; c < names.count(); c++) row.append(values[c * n + r]);
            rows.append(row);
        }
    } else {
        // name, value and units per line
        for (const char *c : { "name", "value", "units" }) columns.append(QJsonObject{ { "name", c }, { "units", "" } });
        for (int r = 0; r < names.count(); r++)
            rows.append(QJsonArray{ names[r], r < values.count() ? values[r] : QString(), r < units.count() ? units[r] : QString() });
    }
    tile.insert("columns", columns);
    tile.insert("rows", rows);
}

static void
zoneTile(AthleteSession &session, RideItem *item, const QJsonObject &config, QJsonObject &tile)
{
    int series = config["series"].toInt();
    bool polarized = config["polarized"].toInt() != 0;
    static const QMap<int, QString> types = { { RideFile::watts, "power" }, { RideFile::hr, "hr" },
                                              { RideFile::kph, "pace" }, { RideFile::wbal, "fatigue" } };
    QString type = types.value(series, "power");
    tile.insert("zones", type);

    // names and times as the tile's bars, percent of the time in all of them
    QStringList names;
    QVector<double> seconds;
    if (polarized && type != "fatigue") {
        QString prefix = type == "hr" ? "time_in_zone_H" : type == "pace" ? "time_in_zone_P" : "time_in_zone_L";
        for (const char *z : { "I", "II", "III" }) { names << z; seconds << std::round(item->getForSymbol(prefix + z)); }
    } else {
        ActivityZones zones;
        QString error;
        if (!activityZones(session.athlete(), item, type, zones, error)) { tile.insert("error", error); return; }
        for (const ZoneRow &r : zones.rows) { names << r.name; seconds << std::round(r.seconds); }
    }
    double sum = 0;
    for (double s : seconds) sum += s;
    QJsonArray rows;
    for (int i = 0; i < names.count(); i++) {
        QJsonObject z;
        z.insert("name", names[i]);
        z.insert("time", time_to_string(seconds[i], true));
        z.insert("seconds", seconds[i]);
        z.insert("percent", sum > 0 ? std::round(seconds[i] / sum * 100) : 0);
        rows.append(z);
    }
    tile.insert("rows", rows);
}

static void
intervalTile(RideItem *item, const QJsonObject &config, bool metricUnits, QJsonObject &tile)
{
    // the bubble chart: one bubble per interval
    QJsonArray axes, rows;
    QStringList symbols = { config["xsymbol"].toString(), config["ysymbol"].toString(), config["zsymbol"].toString() };
    for (const QString &s : symbols) {
        const RideMetric *m = RideMetricFactory::instance().rideMetric(s);
        axes.append(QJsonObject{ { "symbol", s }, { "name", m ? m->name() : s }, { "units", m ? m->units(metricUnits) : QString() } });
    }
    for (IntervalItem *i : item->intervals()) {
        QJsonObject r;
        r.insert("name", i->name);
        for (int a = 0; a < 3; a++) r.insert(QString("xyz").mid(a, 1), i->getStringForSymbol(symbols[a], metricUnits));
        rows.append(r);
    }
    tile.insert("axes", axes);
    tile.insert("rows", rows);
}

static QJsonObject
evaluateTile(AthleteSession &session, RideItem *item, const QJsonObject &config, bool metricUnits)
{
    Context *context = session.context();
    int type = config["type"].toInt();
    QJsonObject tile;
    tile.insert("name", config["name"].toString());

    switch (type) {
    case DATATABLE:
        tile.insert("kind", "table");
        tableTile(context, item, config, tile);
        break;

    case METRIC: {
        QString symbol = config["symbol"].toString();
        const RideMetric *m = RideMetricFactory::instance().rideMetric(symbol);
        QString value = item->getStringForSymbol(symbol, metricUnits);
        tile.insert("kind", "metric");
        tile.insert("symbol", symbol);
        tile.insert("value", value == "nan" ? QString() : value);
        QString units = m ? m->units(metricUnits) : QString();
        tile.insert("units", units == QObject::tr("seconds") ? QString() : units);
        break;
    }

    case META:
        tile.insert("kind", "field");
        tile.insert("field", config["symbol"].toString());
        tile.insert("value", item->getText(config["symbol"].toString(), ""));
        break;

    case RPE:
        tile.insert("kind", "rpe");
        tile.insert("value", item->getText("RPE", "0"));
        break;

    case KPI: {
        DataFilter parser(nullptr, context, Utils::jsonunprotect2(config["program"].toString()));
        QString value = parser.evaluate(item, nullptr).string();
        if (value == "nan") value.clear();
        if (config["istime"].toInt()) value = time_to_string(value.toDouble(), true);
        tile.insert("kind", "kpi");
        tile.insert("value", value);
        tile.insert("units", config["units"].toString());
        if (!parser.getErrors().isEmpty()) tile.insert("error", parser.getErrors().join("; "));
        break;
    }

    case ZONE:
        tile.insert("kind", "zones");
        zoneTile(session, item, config, tile);
        break;

    case PMC: {
        // shown as whole numbers, without a title (PMCOverviewItem)
        QString symbol = config["symbol"].toString();
        PMCData *pmc = pmcFor(session, symbol, -1, -1);
        QDate day = item->dateTime.date();
        tile.insert("kind", "pmc");
        tile.insert("metric", symbol);
        if (pmc) {
            tile.insert("form", std::round(pmc->sb(day)));
            tile.insert("fitness", std::round(pmc->lts(day)));
            tile.insert("fatigue", std::round(pmc->sts(day)));
            tile.insert("risk", std::round(pmc->rr(day)));
        }
        break;
    }

    case INTERVAL:
        tile.insert("kind", "intervals");
        intervalTile(item, config, metricUnits, tile);
        break;

    case ROUTE:
        tile.insert("kind", "route");
        tile.insert("note", "a map, export the track with 'activity export --as gpx'");
        break;

    default:
        tile.insert("kind", "chart");
        tile.insert("note", "a chart, not reproduced as data");
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
tileText(const QJsonObject &tile)
{
    QString text = tile["name"].toString();
    QString kind = tile["kind"].toString();
    if (text.isEmpty()) text = kind == "pmc" ? QString("PMC") : kind;
    text += "\n";
    if (tile.contains("error")) return text + "  error: " + tile["error"].toString() + "\n";

    QList<QStringList> lines;
    if (kind == "table") {
        QJsonArray columns = tile["columns"].toArray();
        bool list = tile["style"].toString() == "list";
        if (!list) {
            QStringList head, units;
            bool anyUnits = false;
            for (const QJsonValue &c : columns) {
                head << c.toObject()["name"].toString();
                units << c.toObject()["units"].toString();
                if (!units.last().isEmpty()) anyUnits = true;
            }
            lines << head;
            if (anyUnits) lines << units;
        }
        for (const QJsonValue &r : tile["rows"].toArray()) {
            QStringList l;
            for (const QJsonValue &v : r.toArray()) l << v.toString();
            lines << l;
        }
    } else if (kind == "zones") {
        for (const QJsonValue &r : tile["rows"].toArray())
            lines << QStringList{ r["name"].toString(), r["time"].toString(), QString("%1 %").arg(r["percent"].toDouble()) };
    } else if (kind == "intervals") {
        QStringList head{ "" };
        for (const QJsonValue &a : tile["axes"].toArray()) head << a["name"].toString();
        lines << head;
        for (const QJsonValue &r : tile["rows"].toArray())
            lines << QStringList{ r["name"].toString(), r["x"].toString(), r["y"].toString(), r["z"].toString() };
    } else if (kind == "pmc") {
        for (const char *k : { "form", "fitness", "fatigue", "risk" })
            lines << QStringList{ QString(k).replace(0, 1, QString(k).at(0).toUpper()), QString::number(tile[k].toDouble()) };
    } else if (tile.contains("value")) {
        lines << QStringList{ tile["value"].toString(), tile["units"].toString() };
    } else {
        lines << QStringList{ tile["note"].toString() };
    }
    return text + columnsText(lines);
}

//
// CSV: one table tile as that table, else a line per value
//

// a data table whose program lists intervals, as the default Intervals tiles do
static bool
intervalProgram(const QString &program)
{
    return program.contains(QLatin1String("intervalstrings(")) || program.contains(QLatin1String("intervals("));
}

static QString
tilesCsv(const QJsonArray &tiles, const QList<bool> &intervalTiles, const QString &censusLine)
{
    auto line = [](const QStringList &f) { return ResultFormat::csvLine(f); };

    if (tiles.count() == 1 && tiles[0]["kind"] == "table" && !tiles[0].toObject().contains("error")) {
        QJsonObject t = tiles[0].toObject();
        QStringList head;
        for (const QJsonValue &c : t["columns"].toArray()) {
            QString units = c["units"].toString();
            head << (units.isEmpty() || t["style"] == "list" ? c["name"].toString() : QString("%1 (%2)").arg(c["name"].toString()).arg(units));
        }
        QString text;
        if (!intervalTiles.isEmpty() && intervalTiles[0]) text += line({ censusLine });
        text += line(head);
        for (const QJsonValue &r : t["rows"].toArray()) {
            QStringList fields;
            for (const QJsonValue &v : r.toArray()) fields << v.toString();
            text += line(fields);
        }
        return text;
    }

    QString text = line({ "tile", "kind", "row", "column", "units", "value" });
    if (intervalTiles.contains(true)) text += line({ "", "summary", "", "", "", censusLine });
    for (const QJsonValue &v : tiles) {
        QJsonObject t = v.toObject();
        QString name = t["name"].toString(), kind = t["kind"].toString();
        auto add = [&](const QString &row, const QString &column, const QString &units, const QString &value) {
            text += line({ name, kind, row, column, units, value });
        };
        if (t.contains("error")) { add("", "error", "", t["error"].toString()); continue; }

        if (kind == "table") {
            QJsonArray columns = t["columns"].toArray();
            QJsonArray rows = t["rows"].toArray();
            for (int r = 0; r < rows.count(); r++) {
                QJsonArray row = rows[r].toArray();
                if (t["style"] == "list") add(row[0].toString(), "value", row[2].toString(), row[1].toString());
                else for (int c = 0; c < row.count() && c < columns.count(); c++)
                    add(QString::number(r + 1), columns[c].toObject()["name"].toString(), columns[c].toObject()["units"].toString(), row[c].toString());
            }
        } else if (kind == "zones") {
            for (const QJsonValue &z : t["rows"].toArray()) {
                add(z["name"].toString(), "time", "", z["time"].toString());
                add(z["name"].toString(), "percent", "%", ResultFormat::csvValue(z["percent"]));
            }
        } else if (kind == "intervals") {
            QJsonArray axes = t["axes"].toArray();
            for (const QJsonValue &r : t["rows"].toArray())
                for (int a = 0; a < 3 && a < axes.count(); a++)
                    add(r["name"].toString(), axes[a].toObject()["name"].toString(), axes[a].toObject()["units"].toString(), r[QString("xyz").mid(a, 1)].toString());
        } else if (kind == "pmc") {
            for (const char *k : { "form", "fitness", "fatigue", "risk" }) add("", k, "", ResultFormat::csvValue(t[k]));
        } else if (t.contains("value")) {
            add("", "value", t["units"].toString(), t["value"].toString());
        } else {
            add("", "note", "", t["note"].toString());
        }
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
    QList<bool> intervalTiles;
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
            QJsonObject tile = evaluateTile(*env.session, item, config, metricUnits);
            tile.insert("chart", chart.title);
            tile.insert("column", config["column"].toInt());
            int tileType = config["type"].toInt();
            bool isIntervals = tileType == INTERVAL
                || (tileType == DATATABLE && intervalProgram(config["program"].toString()));
            tiles.append(tile);
            intervalTiles << isIntervals;
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

    QJsonObject data;
    data.insert("activity", QFileInfo(item->fileName).completeBaseName());
    data.insert("layout", layout.name);
    data.insert("recorded_laps", census.recordedLaps);
    data.insert("user_intervals", census.userIntervals);
    data.insert("discovered_efforts", census.discoveredEfforts);
    data.insert("tiles", tiles);
    CommandResult result = CommandResult::success(data);
    result.csv = tilesCsv(tiles, intervalTiles, censusLine);
    result.text = QString("%1, %2 layout\n\n").arg(activityStart(item).replace("T", " ")).arg(layout.name) + text;
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
}

} // namespace Headless
