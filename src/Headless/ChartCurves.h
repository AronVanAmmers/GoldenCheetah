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

#ifndef _GC_Headless_ChartCurves_h
#define _GC_Headless_ChartCurves_h 1

//
// The curves of a Trends chart: a metric, a best (a duration of one
// series) or an estimate from a CP model, each with its own drawing.
//

#include "HeadlessCommand.h"
#include "LTMSettings.h"
#include "RideFile.h"

#include <QJsonObject>
#include <QStringList>

class Context;

namespace Headless {

// a name for each value of one of the chart's enums
template<class T>
struct Choices {
    QStringList names;
    QList<T> values;

    QString name(T value) const {
        int i = values.indexOf(value);
        return i >= 0 ? names.at(i) : QString::number(int(value));
    }
    // names given are checked against the parameter's oneOf
    T value(const QString &name) const { return values.value(names.indexOf(name), values.first()); }
    // "a, b or c", as the help and the messages put it
    QString either() const { return QStringList(names.mid(0, names.count() - 1)).join(", ") + " or " + names.last(); }
};

const Choices<int> &groups();                           // LTM_DAY ...
const Choices<QwtPlotCurve::CurveStyle> &styles();      // these three in Curve Settings' order
const Choices<QwtSymbol::Style> &markers();
const Choices<RideFile::SeriesType> &bestSeries();
const Choices<int> &durationUnits();                    // in seconds
const Choices<int> &estimates();                        // ESTIMATE_WPRIME ...
const Choices<int> &curveTypes();                       // METRIC_DB ...
QString modelEither();                                  // "cp2, cp3 or ext"

extern const char *const oneCurveDrawing;               // the error for drawing several curves at once

// what a curve plots, as 'chart library show' lists it, and the curve as JSON
QString curveDetail(const MetricDetail &m);
QJsonObject curveJson(const MetricDetail &m, int index);

// the arguments that make curves: --metric as symbols, the kinds refused,
// drawing flags given, how many of --metric, --best and --estimate
bool curveMetrics(const QJsonObject &args, QStringList &symbols, QString &error);
bool metricsFromSymbols(const QStringList &symbols, QList<MetricDetail> &curves, QString &error);
bool supportedTypes(const QJsonObject &args, QString &error);
bool hasDrawing(const QJsonObject &args);
int curveSources(const QJsonObject &args);

// a curve's flags given without the kind of curve they belong to
bool strayCurveArgs(const QJsonObject &args, QString &error);

// how one curve is drawn: the flags given, applied to a curve

struct Drawing {
    bool style = false;
    bool marker = false;
    bool color = false;
    bool fill = false;
    bool filter = false;
    bool units = false;
    QwtPlotCurve::CurveStyle curveStyle = QwtPlotCurve::Lines;
    QwtSymbol::Style symbolStyle = QwtSymbol::NoSymbol;
    QColor pen;
    bool fillCurve = false;
    QString datafilter;
    QString uunits;
};

bool readDrawing(Context *context, const QJsonObject &args, Drawing &drawing, QString &error);
void applyDrawing(MetricDetail &detail, const Drawing &drawing);

// one curve: a metric (symbols from curveMetrics), a best, or an estimate,
// plus any drawing flags that were given; index picks its colour
bool buildCurve(Context *context, const QJsonObject &args, const QStringList &symbols, int index,
                MetricDetail &detail, QString &error);

// the same curve, as far as the file keeps it
bool sameCurve(const MetricDetail &want, const MetricDetail &got);

// the parameters that make a curve
void typedCurveParams(CommandSpec &spec, bool metricRepeated);

} // namespace Headless

#endif
