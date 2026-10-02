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

#ifndef _GC_ChartRenderer_h
#define _GC_ChartRenderer_h 1

//
// Draws charts to an image (PNG), SVG or PDF without a window.
//
// A chart is described as data: one or more panels stacked vertically, each
// with its own series. The renderer knows nothing about athletes or
// activities, the chart commands turn GoldenCheetah data into a ChartSpec.
//

#include <QString>
#include <QStringList>
#include <QVector>
#include <QColor>
#include <QSize>
#include <QList>
#include <QByteArray>
#include <cmath>
#include <functional>

class QPainter;
class QRectF;
class QwtPlot;

namespace Headless {

struct ChartSeries {
    enum Style { Line, Area, Bars, Dots };

    QString name;
    QVector<double> x, y;
    QColor color = QColor(Qt::darkGray);
    Style style = Line;
    double width = 1.5;
    bool rightAxis = false;     // plot against the right hand axis
    bool dashed = false;
};

struct ChartPanel {
    enum XAxis {
        Plain,          // numbers
        Duration,       // seconds shown as h:mm:ss
        LogDuration,    // seconds on a log scale (power-duration curves)
        Date,           // days since 1970-01-01
        Categories      // 0..n-1 labelled with categories
    };

    QString title;
    QString yLabel, yRightLabel;
    XAxis xAxis = Plain;
    QStringList categories;
    QList<ChartSeries> series;
    double yMin = NAN, yMax = NAN;
    double stretch = 1.0;       // share of the chart height
    bool legend = true;
};

struct ChartSpec {
    QString title;
    QString xLabel;             // shown under the last panel
    QList<ChartPanel> panels;
    QSize size = QSize(1200, 600);
    bool dark = false;
};

class ChartRenderer
{
    public:

        static QStringList formats() { return { "png", "svg", "pdf" }; }

        // returns the encoded chart, or an empty array and sets error
        static QByteArray render(const ChartSpec &spec, const QString &format, QString &error);

        // a plot made elsewhere (a Trends chart's LTMPlot), drawn at size as
        // it draws itself, on its own background
        static QByteArray renderPlot(QwtPlot *plot, const QSize &size, const QString &format,
                                     const QString &title, QString &error);

        // what paint draws into the rectangle it is given, encoded as format
        static QByteArray encode(const QSize &size, const QString &format, const QString &title,
                                 const QColor &background,
                                 const std::function<void(QPainter &, const QRectF &)> &paint, QString &error);

        static QString mimeType(const QString &format);
};

} // namespace Headless

#endif
