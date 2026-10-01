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
// metric, a best (a duration of one series) or an estimate from a CP model.
// Drawing flags belong to that one curve. Reading the file leaves out metric
// curves whose metric is not defined any more, so writing it would lose
// them: that is refused unless --drop-unknown says to.
//

#include "HeadlessCommands.h"
#include "ActivitySelection.h"

#include "Athlete.h"
#include "Context.h"
#include "DataFilter.h"
#include "LTMChartParser.h"
#include "LTMSettings.h"
#include "PDModel.h"
#include "RideFile.h"
#include "RideMetric.h"
#include "Utils.h"

#include <QScopeGuard>
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

static const QStringList styleNames = { "bar", "line", "sticks", "dots" };
static const QStringList markerNames = { "none", "circle", "square", "diamond", "triangle", "cross", "hexagon", "star" };
static const QStringList unitNames = { "sec", "min", "hour" };
static const QStringList seriesNames = { "power", "wpk", "xpower", "apower", "isopower", "heartrate", "speed", "cadence", "torque", "vam" };
static const QStringList modelNames = { "cp2", "cp3", "ext" };
static const QStringList estimateNames = { "wprime", "cp", "ftp", "pmax", "best", "ei", "vo2max" };

static const char *oneCurveDrawing =
    "style, symbol, color, fill and filter apply to one curve; add each curve with 'chart library curve add'";

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

// the names below are in the order Curve Settings lists them (MetricDetail)
static QString
styleName(QwtPlotCurve::CurveStyle style)
{
    int i = MetricDetail::curveStyles().indexOf(style);
    return i >= 0 ? styleNames.at(i) : QString::number(int(style));
}

static QwtPlotCurve::CurveStyle
styleFromName(const QString &name)
{
    return MetricDetail::curveStyles().value(styleNames.indexOf(name), QwtPlotCurve::Lines);
}

static QString
markerName(QwtSymbol::Style style)
{
    int i = MetricDetail::symbolStyles().indexOf(style);
    return i >= 0 ? markerNames.at(i) : QString::number(int(style));
}

static QwtSymbol::Style
markerFromName(const QString &name)
{
    return MetricDetail::symbolStyles().value(markerNames.indexOf(name), QwtSymbol::NoSymbol);
}

static QString
unitName(int units)
{
    if (units == 1) return "sec";
    if (units == 60) return "min";
    if (units == 3600) return "hour";
    return QString::number(units);
}

static int
unitSeconds(const QString &name)
{
    switch (unitNames.indexOf(name)) {
    case 0: return 1;
    case 1: return 60;
    case 2: return 3600;
    default: return 0;
    }
}

// seriesNames are in the order of MetricDetail::bestSeries()
static RideFile::SeriesType
seriesFromName(const QString &name)
{
    return MetricDetail::bestSeries().value(seriesNames.indexOf(name), RideFile::none);
}

static QString
seriesToken(RideFile::SeriesType series)
{
    int i = MetricDetail::bestSeries().indexOf(series);
    if (i >= 0) return seriesNames.at(i);
    QString symbol = RideFile::symbolForSeries(series);
    return symbol.isEmpty() ? QString::number(int(series)) : symbol.toLower();
}

// the models Curve Settings offers estimates from, and what each one gives:
// best power, endurance index and VO2max come from every model
static bool
modelOffers(PDModel *model, int estimate)
{
    switch (estimate) {
    case ESTIMATE_WPRIME: return model->hasWPrime();
    case ESTIMATE_CP: return model->hasCP();
    case ESTIMATE_FTP: return model->hasFTP();
    case ESTIMATE_PMAX: return model->hasPMax();
    default: return true;
    }
}

static int
estimateId(const QString &name)
{
    switch (estimateNames.indexOf(name)) {
    case 0: return ESTIMATE_WPRIME;
    case 1: return ESTIMATE_CP;
    case 2: return ESTIMATE_FTP;
    case 3: return ESTIMATE_PMAX;
    case 4: return ESTIMATE_BEST;
    case 5: return ESTIMATE_EI;
    case 6: return ESTIMATE_VO2MAX;
    default: return -1;
    }
}

static QString
estimateToken(int estimate)
{
    if (estimate >= 0 && estimate < estimateNames.count()) return estimateNames.at(estimate);
    return QString::number(estimate);
}

static QString
offersText(PDModel *model)
{
    QStringList offers;
    for (int estimate = 0; estimate < estimateNames.count(); estimate++)
        if (modelOffers(model, estimate)) offers << estimateNames.at(estimate);
    return offers.join(", ");
}

static QString
colorText(const QColor &color)
{
    return color.name(QColor::HexRgb).mid(1).toLower();
}

static bool
parseColor(const QString &text, QColor &color, QString &error)
{
    QString hex = text.trimmed();
    if (hex.startsWith('#')) hex = hex.mid(1);
    if (hex.size() != 6) {
        error = QString("color must be RRGGBB, not '%1'").arg(text);
        return false;
    }
    bool ok = false;
    uint rgb = hex.toUInt(&ok, 16);
    if (!ok) {
        error = QString("color must be RRGGBB, not '%1'").arg(text);
        return false;
    }
    color = QColor((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
    return true;
}

static bool
durationOk(int duration, QString &error)
{
    if (duration < 1 || duration > 999) {
        error = "duration must be from 1 to 999";
        return false;
    }
    return true;
}

static QString
curveDetail(const MetricDetail &m)
{
    if (m.type == METRIC_DB) return m.symbol;
    if (m.type == METRIC_BEST)
        return QString("%1 %2 %3").arg(m.duration).arg(unitName(m.duration_units)).arg(seriesToken(m.series));
    if (m.type == METRIC_ESTIMATE) {
        if (!m.uname.isEmpty()) return m.uname;
        return MetricDetail::estimateName(m.estimate, m.model, m.estimateDuration, m.estimateDuration_units);
    }
    if (!m.uname.isEmpty()) return m.uname;
    if (!m.name.isEmpty()) return m.name;
    return m.symbol;
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
    return LTMSettings::chartsFile(athlete->home->config());
}

// a curve's data filter is kept the way the GUI's filter box stores it:
// "filter:EXPR" for a formula, "search:TEXT" for a free text search, and
// "search:" (or nothing) for no filter. Only a formula can be set here.
static QString
storedFilter(const QString &expression)
{
    return expression.trimmed().isEmpty() ? QString("search:") : "filter:" + expression;
}

static QString
filterExpression(const QString &stored)
{
    return stored.startsWith("filter:") ? stored.mid(7) : QString();
}

static QString
filterSearch(const QString &stored)
{
    if (stored.startsWith("filter:")) return QString();
    return stored.startsWith("search:") ? stored.mid(7) : stored;
}

static QJsonObject
curveJson(const MetricDetail &m, int index)
{
    QJsonObject o;
    o.insert("index", index);
    o.insert("type", curveTypeName(m.type));
    o.insert("detail", curveDetail(m));
    if (!m.symbol.isEmpty()) o.insert("symbol", m.symbol);
    QString formula = metricFormulaName(m.symbol);
    if (!formula.isEmpty()) o.insert("formula", formula);
    if (!m.name.isEmpty()) o.insert("name", m.name);
    if (m.type == METRIC_BEST) {
        o.insert("duration", m.duration);
        o.insert("unit", unitName(m.duration_units));
        o.insert("series", seriesToken(m.series));
    }
    if (m.type == METRIC_ESTIMATE) {
        o.insert("model", m.model);
        o.insert("estimate", estimateToken(m.estimate));
        if (m.estimate == ESTIMATE_BEST) {
            o.insert("duration", m.estimateDuration);
            o.insert("unit", unitName(m.estimateDuration_units));
        }
    }
    o.insert("style", styleName(m.curveStyle));
    o.insert("marker", markerName(m.symbolStyle));
    o.insert("color", colorText(m.penColor));
    o.insert("fill", m.fillCurve);
    QString expression = filterExpression(m.datafilter);
    o.insert("filter", expression.isEmpty() ? QJsonValue() : QJsonValue(expression));
    QString search = filterSearch(m.datafilter);
    if (!search.isEmpty()) o.insert("search", search);
    return o;
}

static QJsonObject
chartJson(const LTMSettings &chart)
{
    QJsonArray metrics;
    for (int i = 0; i < chart.metrics.count(); i++) metrics.append(curveJson(chart.metrics.at(i), i + 1));
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
    for (int i = 0; i < chart.metrics.count(); i++) {
        const MetricDetail &m = chart.metrics.at(i);
        text += QString("  %1  %2  %3  %4  %5\n")
            .arg(i + 1)
            .arg(curveTypeName(m.type))
            .arg(curveDetail(m))
            .arg(styleName(m.curveStyle))
            .arg(markerName(m.symbolStyle));
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

// as LTMTool's catalogue has it, with our palette
static MetricDetail
metricCurve(const RideMetric *metric, int index)
{
    MetricDetail detail = MetricDetail::forMetric(metric, GlobalContext::context()->useMetricUnits);
    detail.type = METRIC_DB;
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
metricFromSymbol(const QString &symbol, int index, MetricDetail &detail, QString &error)
{
    if (symbol.startsWith("compatibility_")) {
        error = QString("metric '%1' is only kept for old charts, see 'metric list'").arg(symbol);
        return false;
    }
    const RideMetric *metric = RideMetricFactory::instance().rideMetric(symbol);
    if (!metric) {
        error = QString("unknown metric '%1', see 'metric list'").arg(symbol);
        return false;
    }
    detail = metricCurve(metric, index);
    return true;
}

static bool
metricsFromArgs(const QJsonObject &args, QList<MetricDetail> &curves, QString &error)
{
    QStringList symbols;
    if (!resolveMetrics(splitList(args.value("metric")), symbols, error)) return false;
    if (symbols.isEmpty()) {
        error = "give a metric with --metric";
        return false;
    }
    for (int i = 0; i < symbols.count(); i++) {
        MetricDetail detail;
        if (!metricFromSymbol(symbols.at(i), i, detail, error)) return false;
        curves << detail;
    }
    return true;
}

static bool
supportedTypes(const QJsonObject &args, QString &error)
{
    struct { const char *flag; const char *label; } kinds[] = {
        { "pmc", "PMC" },
        { "banister", "Banister" },
        { "performance", "performance" },
        { "formula", "formula" },
        { "measure", "measure" },
    };
    for (const auto &k : kinds) {
        if (!args.contains(k.flag)) continue;
        error = QString("%1 curves are not supported").arg(k.label);
        return false;
    }
    return true;
}

static bool
hasDrawing(const QJsonObject &args)
{
    return args.contains("style") || args.contains("symbol") || args.contains("color")
        || args.contains("fill") || args.contains("filter");
}

static int
curveSources(const QJsonObject &args)
{
    return int(args.contains("metric")) + int(args.contains("best")) + int(args.contains("estimate"));
}

static bool
checkFilter(Context *context, const QString &expr, QString &error)
{
    if (expr.trimmed().isEmpty()) return true;
    DataFilter checker(nullptr, context);
    QStringList errors = checker.check(expr);
    if (!errors.isEmpty() || !checker.root()) {
        if (errors.isEmpty()) errors << QString("malformed expression.");
        error = errors.join("\n");
        return false;
    }
    return true;
}

struct Drawing {
    bool style = false;
    bool symbol = false;
    bool color = false;
    bool fill = false;
    bool filter = false;
    QwtPlotCurve::CurveStyle curveStyle = QwtPlotCurve::Lines;
    QwtSymbol::Style symbolStyle = QwtSymbol::NoSymbol;
    QColor pen;
    bool fillCurve = false;
    QString datafilter;
};

static bool
readDrawing(Context *context, const QJsonObject &args, Drawing &drawing, QString &error)
{
    if (args.contains("style")) {
        drawing.style = true;
        drawing.curveStyle = styleFromName(args.value("style").toString());
    }
    if (args.contains("symbol")) {
        drawing.symbol = true;
        drawing.symbolStyle = markerFromName(args.value("symbol").toString());
    }
    if (args.contains("color")) {
        drawing.color = true;
        if (!parseColor(args.value("color").toString(), drawing.pen, error)) return false;
    }
    if (args.contains("fill")) {
        drawing.fill = true;
        drawing.fillCurve = args.value("fill").toBool();
    }
    if (args.contains("filter")) {
        drawing.filter = true;
        QString expression = args.value("filter").toString();
        if (!checkFilter(context, expression, error)) return false;
        drawing.datafilter = storedFilter(expression);
    }
    return true;
}

static void
applyDrawing(MetricDetail &detail, const Drawing &drawing)
{
    if (drawing.style) detail.curveStyle = drawing.curveStyle;
    if (drawing.symbol) detail.symbolStyle = drawing.symbolStyle;
    if (drawing.color) {
        detail.penColor = drawing.pen;
        detail.brushColor = drawing.pen;
    }
    if (drawing.fill) detail.fillCurve = drawing.fillCurve;
    if (drawing.filter) detail.datafilter = drawing.datafilter;
}

static void
prepareCurve(MetricDetail &detail, int index)
{
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
    detail.penColor = penColor(index);
    detail.brushColor = detail.penColor;
    detail.curveStyle = QwtPlotCurve::Lines;
    detail.symbolStyle = QwtSymbol::NoSymbol;
    detail.fillCurve = false;
}

static bool
strayCurveArgs(const QJsonObject &args, QString &error)
{
    bool best = args.contains("best");
    bool estimateBest = args.contains("estimate") && args.value("estimate").toString() == "best";
    if (args.contains("series") && !best) {
        error = "--series is part of a best curve";
        return false;
    }
    if (args.contains("model") && !args.contains("estimate")) {
        error = "--model is part of an estimate curve";
        return false;
    }
    if (args.contains("duration") && !estimateBest) {
        error = "--duration is the length of an estimate of best power";
        return false;
    }
    if (args.contains("unit") && !best && !estimateBest) {
        error = "--unit is the duration unit of a best, or of an estimate of best power";
        return false;
    }
    if (best && args.contains("duration")) {
        error = "--best is the duration; --duration is for an estimate of best power";
        return false;
    }
    return true;
}

static bool
buildBest(const QJsonObject &args, int index, MetricDetail &detail, QString &error)
{
    int duration = args.value("best").toInt();
    if (!durationOk(duration, error)) return false;
    if (!args.contains("unit")) {
        error = "best needs --unit sec, min or hour";
        return false;
    }
    int units = unitSeconds(args.value("unit").toString());
    if (!units) {
        error = "unit must be sec, min or hour";
        return false;
    }
    if (!args.contains("series")) {
        error = QString("best needs --series (%1)").arg(seriesNames.join(", "));
        return false;
    }
    RideFile::SeriesType series = seriesFromName(args.value("series").toString());
    if (series == RideFile::none) {
        error = QString("series must be one of: %1").arg(seriesNames.join(", "));
        return false;
    }

    prepareCurve(detail, index);
    detail.type = METRIC_BEST;
    detail.duration = duration;
    detail.duration_units = units;
    detail.series = series;
    QString uname = MetricDetail::bestName(duration, units, series);
    detail.uname = uname;
    detail.name = uname;
    detail.bestSymbol = QString(uname).replace(" ", "_");
    return true;
}

static bool
buildEstimate(Context *context, const QJsonObject &args, int index, MetricDetail &detail, QString &error)
{
    if (!args.contains("model")) {
        error = "estimate needs --model cp2, cp3 or ext";
        return false;
    }
    QList<PDModel *> models = MetricDetail::estimateModels(context);
    auto cleanup = qScopeGuard([&models]() { qDeleteAll(models); });
    PDModel *model = nullptr;
    for (PDModel *m : models) if (m->code() == args.value("model").toString()) model = m;
    if (!model) {
        error = "model must be cp2, cp3 or ext";
        return false;
    }
    int estimate = estimateId(args.value("estimate").toString());
    if (estimate < 0) {
        error = QString("estimate must be one of: %1").arg(estimateNames.join(", "));
        return false;
    }
    if (!modelOffers(model, estimate)) {
        error = QString("the %1 model does not offer %2; it offers %3")
            .arg(model->code(), estimateToken(estimate), offersText(model));
        return false;
    }

    int duration = 0;
    int units = 1;
    if (estimate == ESTIMATE_BEST) {
        if (!args.contains("duration")) {
            error = "an estimate of best power needs --duration";
            return false;
        }
        duration = args.value("duration").toInt();
        if (!durationOk(duration, error)) return false;
        if (!args.contains("unit")) {
            error = "an estimate of best power needs --unit sec, min or hour";
            return false;
        }
        units = unitSeconds(args.value("unit").toString());
        if (!units) {
            error = "unit must be sec, min or hour";
            return false;
        }
    }

    prepareCurve(detail, index);
    detail.type = METRIC_ESTIMATE;
    detail.model = model->code();
    detail.estimate = estimate;
    detail.estimateDuration = duration;
    detail.estimateDuration_units = units;
    QString uname = MetricDetail::estimateName(estimate, model->code(), duration, units);
    detail.uname = uname;
    detail.name = uname;
    detail.symbol = QString(uname).replace(" ", "_");
    return true;
}

// one curve: a metric, a best, or an estimate, plus any drawing flags that were given
static bool
buildCurve(Context *context, const QJsonObject &args, int index, MetricDetail &detail, QString &error)
{
    if (!strayCurveArgs(args, error)) return false;
    Drawing drawing;
    if (!readDrawing(context, args, drawing, error)) return false;

    if (args.contains("metric")) {
        QStringList symbols;
        if (!resolveMetrics(splitList(args.value("metric")), symbols, error)) return false;
        if (symbols.count() != 1) {
            error = oneCurveDrawing;
            return false;
        }
        if (!metricFromSymbol(symbols.first(), index, detail, error)) return false;
    } else if (args.contains("best")) {
        if (!buildBest(args, index, detail, error)) return false;
    } else if (args.contains("estimate")) {
        if (!buildEstimate(context, args, index, detail, error)) return false;
    } else {
        error = "give a curve with --metric, --best or --estimate";
        return false;
    }
    applyDrawing(detail, drawing);
    return true;
}

static bool
sameCurve(const MetricDetail &want, const MetricDetail &got)
{
    if (want.type != got.type || want.symbol != got.symbol) return false;
    if (want.curveStyle != got.curveStyle || want.symbolStyle != got.symbolStyle) return false;
    if ((want.penColor.rgb() & 0x00ffffff) != (got.penColor.rgb() & 0x00ffffff)) return false;
    if (want.fillCurve != got.fillCurve || want.datafilter != got.datafilter) return false;
    if (want.type == METRIC_BEST) {
        if (want.duration != got.duration || want.duration_units != got.duration_units) return false;
        if (want.series != got.series || want.bestSymbol != got.bestSymbol) return false;
    }
    if (want.type == METRIC_ESTIMATE) {
        if (want.model != got.model || want.estimate != got.estimate) return false;
        if (want.estimate == ESTIMATE_BEST &&
            (want.estimateDuration != got.estimateDuration || want.estimateDuration_units != got.estimateDuration_units))
            return false;
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

    QString error;
    if (!roundTrip(charts, error)) return CommandResult::failure(Status::Usage, error);
    if (!athlete->saveCharts(charts, &error)) return CommandResult::failure(Status::Failed, error);
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

static bool
replacementCurves(Context *context, const QJsonObject &args, QList<MetricDetail> &curves, QString &error)
{
    if (!supportedTypes(args, error)) return false;
    int sources = curveSources(args);
    if (sources == 0) {
        error = "give a curve with --metric, --best or --estimate";
        return false;
    }
    if (sources > 1) {
        error = "give only one of --metric, --best and --estimate";
        return false;
    }
    if (args.contains("metric")) {
        QStringList symbols;
        if (!resolveMetrics(splitList(args.value("metric")), symbols, error)) return false;
        if (symbols.count() != 1) {
            if (hasDrawing(args)) {
                error = oneCurveDrawing;
                return false;
            }
            if (!strayCurveArgs(args, error)) return false;
            return metricsFromArgs(args, curves, error);
        }
    }
    MetricDetail detail;
    if (!buildCurve(context, args, 0, detail, error)) return false;
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

    int by = groupId(request.args.value("by").toString());
    if (!by) return CommandResult::failure(Status::Usage, "group by must be day, week, month, year, tod or all");

    QList<MetricDetail> curves;
    QString error;
    if (!replacementCurves(env.session->context(), request.args, curves, error))
        return CommandResult::failure(Status::Usage, error);

    QList<LTMSettings> charts = athlete->presets;
    LTMSettings chart = blankChart(name, by);
    chart.metrics = curves;
    charts.append(chart);

    CommandResult written = writeCharts(athlete, charts, request.args);
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
    bool regroup = request.args.contains("by");
    bool replace = curveSources(request.args) > 0;
    bool drawing = hasDrawing(request.args);
    QString error;
    if (!supportedTypes(request.args, error)) return CommandResult::failure(Status::Usage, error);
    if (!rename && !regroup && !replace && !drawing)
        return CommandResult::failure(Status::Usage, "give --name, --by, or a curve with --metric, --best or --estimate");
    if (drawing && !replace)
        return CommandResult::failure(Status::Usage, "to change one curve, use 'chart library curve edit'");

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
    if (regroup) {
        int by = groupId(request.args.value("by").toString());
        if (!by) return CommandResult::failure(Status::Usage, "group by must be day, week, month, year, tod or all");
        chart.groupBy = by;
    }
    if (replace) {
        QList<MetricDetail> curves;
        if (!replacementCurves(env.session->context(), request.args, curves, error))
            return CommandResult::failure(Status::Usage, error);
        chart.metrics = curves;
    }

    CommandResult written = writeCharts(athlete, charts, request.args);
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
    CommandResult written = writeCharts(athlete, charts, request.args);
    if (!written.ok()) return written;

    QJsonObject data;
    data.insert("status", "removed");
    data.insert("name", name);
    data.insert("file", chartsPath(athlete));
    CommandResult result = CommandResult::success(data);
    result.text = QString("removed %1\n").arg(name);
    return result;
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
    MetricDetail detail;
    if (!buildCurve(env.session->context(), request.args, chart.metrics.count(), detail, error))
        return CommandResult::failure(Status::Usage, error);
    chart.metrics.append(detail);

    CommandResult written = writeCharts(athlete, charts, request.args);
    if (!written.ok()) return written;

    QJsonObject data = chartJson(charts.at(chartIndex));
    data.insert("status", "added");
    data.insert("file", chartsPath(athlete));
    CommandResult result = CommandResult::success(data);
    result.text = QString("added curve %1 on %2\n").arg(chart.metrics.count()).arg(chart.name);
    return result;
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
        MetricDetail detail;
        if (!buildCurve(env.session->context(), request.args, index, detail, error))
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

    CommandResult written = writeCharts(athlete, charts, request.args);
    if (!written.ok()) return written;

    QJsonObject data = chartJson(charts.at(chartIndex));
    data.insert("status", "updated");
    data.insert("file", chartsPath(athlete));
    CommandResult result = CommandResult::success(data);
    result.text = QString("updated curve %1 on %2\n").arg(index + 1).arg(chart.name);
    return result;
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

    CommandResult written = writeCharts(athlete, charts, request.args);
    if (!written.ok()) return written;

    QJsonObject data = chartJson(charts.at(chartIndex));
    data.insert("status", "removed");
    data.insert("file", chartsPath(athlete));
    CommandResult result = CommandResult::success(data);
    result.text = QString("removed curve %1 on %2\n").arg(index + 1).arg(chart.name);
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

static void
drawingParams(CommandSpec &spec)
{
    spec.params << ParamSpec("style", ParamType::String, "how the curve is drawn").oneOf(styleNames);
    spec.params << ParamSpec("symbol", ParamType::String, "marker drawn on the curve").oneOf(markerNames);
    spec.params << ParamSpec("color", ParamType::String, "pen color as RRGGBB");
    spec.params << ParamSpec("fill", ParamType::Bool, "fill under the curve");
    spec.params << ParamSpec("filter", ParamType::String, "curve data filter, such as isRun; \"\" for none");
}

// every command that writes the file
static void
writeParams(CommandSpec &spec)
{
    spec.params << ParamSpec("drop-unknown", ParamType::Bool,
                             "save even though curves whose metric is not defined are lost");
}

static void
refusedParams(CommandSpec &spec)
{
    spec.params << ParamSpec("pmc", ParamType::Bool, "not supported");
    spec.params << ParamSpec("banister", ParamType::Bool, "not supported");
    spec.params << ParamSpec("performance", ParamType::Bool, "not supported");
    spec.params << ParamSpec("formula", ParamType::Bool, "not supported");
    spec.params << ParamSpec("measure", ParamType::Bool, "not supported");
}

static void
typedCurveParams(CommandSpec &spec, bool metricRepeated)
{
    ParamSpec metric("metric", ParamType::String, "metric symbol or formula name, from 'metric list'");
    if (metricRepeated) metric.many();
    spec.params << metric;
    spec.params << ParamSpec("best", ParamType::Int, "peak duration; also give --unit and --series");
    spec.params << ParamSpec("unit", ParamType::String, "duration unit").oneOf(unitNames);
    spec.params << ParamSpec("series", ParamType::String, "series for a best; power is the one CP Analysis uses").oneOf(seriesNames);
    spec.params << ParamSpec("estimate", ParamType::String, "wprime, cp, ftp, pmax, best, ei or vo2max").oneOf(estimateNames);
    spec.params << ParamSpec("model", ParamType::String, "cp2, cp3 or ext").oneOf(modelNames);
    spec.params << ParamSpec("duration", ParamType::Int, "length of an estimate of best power");
    drawingParams(spec);
    refusedParams(spec);
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
        "or an estimate from a CP model (cp2, cp3 or ext). Several --metric flags\n"
        "and no drawing flags keep today's metric curves. A style, symbol, color,\n"
        "fill or filter applies to one curve; add further curves with\n"
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
