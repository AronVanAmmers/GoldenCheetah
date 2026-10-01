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
// The curves of a Trends chart (see ChartLibraryCommands.cpp): their names
// and kinds, how they are built from command arguments and shown.
//

#include "ChartCurves.h"
#include "ActivitySelection.h"
#include "MetricNames.h"

#include "Context.h"
#include "DataFilter.h"
#include "PDModel.h"
#include "RideMetric.h"

#include <QScopeGuard>

namespace Headless {

const Choices<int> &
groups()
{
    static const Choices<int> c{ { "day", "week", "month", "year", "tod", "all" },
                                 { LTM_DAY, LTM_WEEK, LTM_MONTH, LTM_YEAR, LTM_TOD, LTM_ALL } };
    return c;
}

// styles, markers and best series are in the order Curve Settings lists them
const Choices<QwtPlotCurve::CurveStyle> &
styles()
{
    static const Choices<QwtPlotCurve::CurveStyle> c{ { "bar", "line", "sticks", "dots" }, MetricDetail::curveStyles() };
    return c;
}

const Choices<QwtSymbol::Style> &
markers()
{
    static const Choices<QwtSymbol::Style> c{ { "none", "circle", "square", "diamond", "triangle", "cross", "hexagon", "star" },
                                              MetricDetail::symbolStyles() };
    return c;
}

const Choices<RideFile::SeriesType> &
bestSeries()
{
    static const Choices<RideFile::SeriesType> c{
        { "power", "wpk", "xpower", "apower", "isopower", "heartrate", "speed", "cadence", "torque", "vam" },
        MetricDetail::bestSeries() };
    return c;
}

const Choices<int> &
durationUnits()
{
    static const Choices<int> c{ { "sec", "min", "hour" }, { 1, 60, 3600 } };
    return c;
}

const Choices<int> &
estimates()
{
    static const Choices<int> c{ { "wprime", "cp", "ftp", "pmax", "best", "ei", "vo2max" },
                                 { ESTIMATE_WPRIME, ESTIMATE_CP, ESTIMATE_FTP, ESTIMATE_PMAX, ESTIMATE_BEST, ESTIMATE_EI, ESTIMATE_VO2MAX } };
    return c;
}

const Choices<int> &
curveTypes()
{
    static const Choices<int> c{ { "metric", "pmc", "meta", "best", "estimate", "stress", "formula", "measure", "performance", "banister" },
                                 { METRIC_DB, METRIC_PM, METRIC_META, METRIC_BEST, METRIC_ESTIMATE, METRIC_STRESS,
                                   METRIC_FORMULA, METRIC_D_MEASURE, METRIC_PERFORMANCE, METRIC_BANISTER } };
    return c;
}

// the models Curve Settings offers estimates from (MetricDetail::estimateModels)
static const QStringList modelNames = { "cp2", "cp3", "ext" };

QString
modelEither()
{
    return Choices<int>{ modelNames, { 0, 0, 0 } }.either();
}

const char *const oneCurveDrawing =
    "style, marker, color, fill, filter and units apply to one curve; add each curve with 'chart library curve add'";

static QString
seriesToken(RideFile::SeriesType series)
{
    if (bestSeries().values.contains(series)) return bestSeries().name(series);
    QString symbol = RideFile::symbolForSeries(series);
    return symbol.isEmpty() ? QString::number(int(series)) : symbol.toLower();
}

// what each model gives: best power, endurance index and VO2max come from every model
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

static QString
offersText(PDModel *model)
{
    QStringList offers;
    for (int estimate : estimates().values)
        if (modelOffers(model, estimate)) offers << estimates().name(estimate);
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

// the units a curve is drawn against, as the built-in charts have them:
// curves with the same units share an axis, so every kind of power is
// "Watts" (CP Analysis puts bests and CP on one axis)
static QString
bestUnits(RideFile::SeriesType series, Context *context)
{
    switch (series) {
    case RideFile::watts: case RideFile::xPower: case RideFile::aPower: case RideFile::IsoPower: return "Watts";
    case RideFile::wattsKg: return "Watts/kg";
    default: return RideFile::unitName(series, context);
    }
}

static QString
estimateUnits(int estimate, bool wpk)
{
    switch (estimate) {
    case ESTIMATE_WPRIME: return wpk ? "Joules/kg" : "Joules";
    case ESTIMATE_CP: case ESTIMATE_FTP: case ESTIMATE_PMAX: case ESTIMATE_BEST: return wpk ? "Watts/kg" : "Watts";
    case ESTIMATE_VO2MAX: return "ml/min/kg";
    default: return QString();      // the endurance index has none
    }
}

QString
curveDetail(const MetricDetail &m)
{
    if (m.type == METRIC_DB) return m.symbol;
    if (m.type == METRIC_BEST)
        return QString("%1 %2 %3").arg(m.duration).arg(durationUnits().name(m.duration_units)).arg(seriesToken(m.series));
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

QJsonObject
curveJson(const MetricDetail &m, int index)
{
    QJsonObject o;
    o.insert("index", index);
    o.insert("type", curveTypes().name(m.type));
    o.insert("detail", curveDetail(m));
    if (!m.symbol.isEmpty()) o.insert("symbol", m.symbol);
    QString formula = metricFormulaName(m.symbol);
    if (!formula.isEmpty()) o.insert("formula", formula);
    if (!m.name.isEmpty()) o.insert("name", m.name);
    if (m.type == METRIC_BEST) {
        o.insert("duration", m.duration);
        o.insert("unit", durationUnits().name(m.duration_units));
        o.insert("series", seriesToken(m.series));
    }
    if (m.type == METRIC_ESTIMATE) {
        o.insert("model", m.model);
        o.insert("estimate", estimates().name(m.estimate));
        o.insert("wpk", m.wpk);
        if (m.estimate == ESTIMATE_BEST) {
            o.insert("duration", m.estimateDuration);
            o.insert("unit", durationUnits().name(m.estimateDuration_units));
        }
    }
    // the axis a curve goes on is chosen by these
    o.insert("units", m.uunits);
    o.insert("style", styles().name(m.curveStyle));
    o.insert("marker", markers().name(m.symbolStyle));
    o.insert("color", colorText(m.penColor));
    o.insert("fill", m.fillCurve);
    QString expression = filterExpression(m.datafilter);
    o.insert("filter", expression.isEmpty() ? QJsonValue() : QJsonValue(expression));
    QString search = filterSearch(m.datafilter);
    if (!search.isEmpty()) o.insert("search", search);
    return o;
}

// what every new curve starts with, in our palette
static void
resetCurve(MetricDetail &detail, int index)
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
}

// as LTMTool's catalogue has it, drawn as it draws that kind of metric
static MetricDetail
metricCurve(const RideMetric *metric, int index)
{
    MetricDetail detail = MetricDetail::forMetric(metric, GlobalContext::context()->useMetricUnits);
    detail.type = METRIC_DB;
    resetCurve(detail, index);
    detail.formulaType = metric->type();
    return detail;
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

// --metric, as symbols
bool
curveMetrics(const QJsonObject &args, QStringList &symbols, QString &error)
{
    symbols.clear();
    if (!args.contains("metric")) return true;
    return resolveMetrics(splitList(args.value("metric")), symbols, error);
}

bool
metricsFromSymbols(const QStringList &symbols, QList<MetricDetail> &curves, QString &error)
{
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

bool
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

bool
hasDrawing(const QJsonObject &args)
{
    return args.contains("style") || args.contains("marker") || args.contains("color")
        || args.contains("fill") || args.contains("filter") || args.contains("units");
}

int
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

bool
readDrawing(Context *context, const QJsonObject &args, Drawing &drawing, QString &error)
{
    if (args.contains("style")) {
        drawing.style = true;
        drawing.curveStyle = styles().value(args.value("style").toString());
    }
    if (args.contains("marker")) {
        drawing.marker = true;
        drawing.symbolStyle = markers().value(args.value("marker").toString());
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
    if (args.contains("units")) {
        drawing.units = true;
        drawing.uunits = args.value("units").toString();
    }
    return true;
}

void
applyDrawing(MetricDetail &detail, const Drawing &drawing)
{
    if (drawing.style) detail.curveStyle = drawing.curveStyle;
    if (drawing.marker) detail.symbolStyle = drawing.symbolStyle;
    if (drawing.color) {
        detail.penColor = drawing.pen;
        detail.brushColor = drawing.pen;
    }
    if (drawing.fill) detail.fillCurve = drawing.fillCurve;
    if (drawing.filter) detail.datafilter = drawing.datafilter;
    if (drawing.units) {
        detail.uunits = drawing.uunits;
        detail.units = drawing.uunits;
    }
}

// a best or an estimate: a plain line
static void
prepareCurve(MetricDetail &detail, int index)
{
    resetCurve(detail, index);
    detail.curveStyle = QwtPlotCurve::Lines;
    detail.symbolStyle = QwtSymbol::NoSymbol;
    detail.fillCurve = false;
}

bool
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
    if (args.contains("wpk") && !args.contains("estimate")) {
        error = "--wpk is for an estimate curve; a best of watts per kilogram is --series wpk";
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

// a duration and its --unit: --best's, or --duration of an estimate of best power
static bool
readDuration(const QJsonObject &args, const QString &key, const QString &what, int &duration, int &units, QString &error)
{
    if (!args.contains(key)) {
        error = QString("%1 needs --%2").arg(what, key);
        return false;
    }
    duration = args.value(key).toInt();
    if (!durationOk(duration, error)) return false;
    if (!args.contains("unit")) {
        error = QString("%1 needs --unit %2").arg(what, durationUnits().either());
        return false;
    }
    units = durationUnits().value(args.value("unit").toString());
    return true;
}

static bool
buildBest(Context *context, const QJsonObject &args, int index, MetricDetail &detail, QString &error)
{
    int duration = 0, units = 1;
    if (!readDuration(args, "best", "best", duration, units, error)) return false;
    if (!args.contains("series")) {
        error = QString("best needs --series (%1)").arg(bestSeries().names.join(", "));
        return false;
    }
    RideFile::SeriesType series = bestSeries().value(args.value("series").toString());

    prepareCurve(detail, index);
    detail.type = METRIC_BEST;
    detail.duration = duration;
    detail.duration_units = units;
    detail.series = series;
    QString uname = MetricDetail::bestName(duration, units, series);
    detail.uname = uname;
    detail.name = uname;
    detail.bestSymbol = QString(uname).replace(" ", "_");
    detail.uunits = detail.units = bestUnits(series, context);
    return true;
}

static bool
buildEstimate(Context *context, const QJsonObject &args, int index, MetricDetail &detail, QString &error)
{
    if (!args.contains("model")) {
        error = "estimate needs --model " + modelEither();
        return false;
    }
    QList<PDModel *> models = MetricDetail::estimateModels(context);
    auto cleanup = qScopeGuard([&models]() { qDeleteAll(models); });
    PDModel *model = nullptr;
    for (PDModel *m : models) if (m->code() == args.value("model").toString()) model = m;
    if (!model) {
        error = "model must be " + modelEither();
        return false;
    }
    int estimate = estimates().value(args.value("estimate").toString());
    if (!modelOffers(model, estimate)) {
        error = QString("the %1 model does not offer %2; it offers %3")
            .arg(model->code(), estimates().name(estimate), offersText(model));
        return false;
    }

    int duration = 0, units = 1;
    if (estimate == ESTIMATE_BEST
        && !readDuration(args, "duration", "an estimate of best power", duration, units, error)) return false;

    prepareCurve(detail, index);
    detail.type = METRIC_ESTIMATE;
    detail.model = model->code();
    detail.estimate = estimate;
    detail.estimateDuration = duration;
    detail.estimateDuration_units = units;
    // per kilogram, as Curve Settings' Absolute / Per Kilogram
    detail.wpk = args.value("wpk").toBool(false);
    QString uname = MetricDetail::estimateName(estimate, model->code(), duration, units);
    detail.uname = uname;
    detail.name = uname;
    detail.symbol = QString(uname).replace(" ", "_");
    detail.uunits = detail.units = estimateUnits(estimate, detail.wpk);
    return true;
}

// one curve: a metric, a best, or an estimate, plus any drawing flags that were given
bool
buildCurve(Context *context, const QJsonObject &args, const QStringList &symbols, int index, MetricDetail &detail, QString &error)
{
    if (!strayCurveArgs(args, error)) return false;
    Drawing drawing;
    if (!readDrawing(context, args, drawing, error)) return false;

    if (args.contains("metric")) {
        if (symbols.count() != 1) {
            error = oneCurveDrawing;
            return false;
        }
        if (!metricFromSymbol(symbols.first(), index, detail, error)) return false;
    } else if (args.contains("best")) {
        if (!buildBest(context, args, index, detail, error)) return false;
    } else if (args.contains("estimate")) {
        if (!buildEstimate(context, args, index, detail, error)) return false;
    } else {
        error = "give a curve with --metric, --best or --estimate";
        return false;
    }
    applyDrawing(detail, drawing);
    return true;
}

bool
sameCurve(const MetricDetail &want, const MetricDetail &got)
{
    if (want.type != got.type || want.symbol != got.symbol) return false;
    if (want.curveStyle != got.curveStyle || want.symbolStyle != got.symbolStyle) return false;
    if ((want.penColor.rgb() & 0x00ffffff) != (got.penColor.rgb() & 0x00ffffff)) return false;
    if (want.fillCurve != got.fillCurve || want.datafilter != got.datafilter) return false;
    if (want.uunits != got.uunits || want.uname != got.uname || want.wpk != got.wpk) return false;
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

static void
drawingParams(CommandSpec &spec)
{
    spec.params << ParamSpec("style", ParamType::String, "how the curve is drawn").oneOf(styles().names);
    spec.params << ParamSpec("marker", ParamType::String, "marker drawn on the curve").oneOf(markers().names);
    spec.params << ParamSpec("color", ParamType::String, "pen color as RRGGBB");
    spec.params << ParamSpec("fill", ParamType::Bool, "fill under the curve");
    spec.params << ParamSpec("filter", ParamType::String, "curve data filter, such as isRun; \"\" for none");
    spec.params << ParamSpec("units", ParamType::String, "units of the curve's axis; curves with the same units share one");
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

void
typedCurveParams(CommandSpec &spec, bool metricRepeated)
{
    ParamSpec metric("metric", ParamType::String, "metric symbol or formula name, from 'metric list'");
    if (metricRepeated) metric.many();
    spec.params << metric;
    spec.params << ParamSpec("best", ParamType::Int, "peak duration; also give --unit and --series");
    spec.params << ParamSpec("unit", ParamType::String, "duration unit").oneOf(durationUnits().names);
    spec.params << ParamSpec("series", ParamType::String, "series for a best; power is the one CP Analysis uses").oneOf(bestSeries().names);
    spec.params << ParamSpec("estimate", ParamType::String, estimates().either()).oneOf(estimates().names);
    spec.params << ParamSpec("model", ParamType::String, modelEither()).oneOf(modelNames);
    spec.params << ParamSpec("duration", ParamType::Int, "length of an estimate of best power");
    spec.params << ParamSpec("wpk", ParamType::Bool, "an estimate per kilogram instead of absolute");
    drawingParams(spec);
    refusedParams(spec);
}

} // namespace Headless
