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

#include "ChartRenderer.h"

#include <qwt_plot.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_grid.h>
#include <qwt_plot_renderer.h>
#include <qwt_plot_layout.h>
#include <qwt_plot_canvas.h>
#include <qwt_legend.h>
#include <qwt_scale_draw.h>
#include <qwt_scale_engine.h>
#include <qwt_scale_widget.h>
#include <qwt_text.h>
#include <qwt_plot_barchart.h>
#include <qwt_column_symbol.h>
#include <qwt_symbol.h>

#include <QBuffer>
#include <QDate>
#include <QImage>
#include <QPainter>
#include <QPdfWriter>
#include <QSvgGenerator>
#include <memory>

namespace Headless {

// seconds as h:mm:ss / m:ss
static QString
durationLabel(double secs, bool shortForm)
{
    int s = int(std::round(secs));
    int h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    if (shortForm) {
        // power-duration style: 5s, 1m, 20m, 1h
        if (s < 60) return QString("%1s").arg(s);
        if (s < 3600) return sec ? QString("%1m%2s").arg(m).arg(sec) : QString("%1m").arg(m);
        return m ? QString("%1h%2m").arg(h).arg(m) : QString("%1h").arg(h);
    }
    if (h) return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
    return QString("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
}

class DurationScaleDraw : public QwtScaleDraw
{
    public:
        explicit DurationScaleDraw(bool shortForm) : shortForm(shortForm) {}
        QwtText label(double v) const override { return QwtText(durationLabel(v, shortForm)); }
    private:
        bool shortForm;
};

class DateScaleDraw : public QwtScaleDraw
{
    public:
        QwtText label(double v) const override {
            return QwtText(QDate(1970, 1, 1).addDays(qint64(std::round(v))).toString("d MMM yy"));
        }
};

class CategoryScaleDraw : public QwtScaleDraw
{
    public:
        explicit CategoryScaleDraw(const QStringList &names) : names(names) {
            enableComponent(QwtScaleDraw::Ticks, false);
        }
        QwtText label(double v) const override {
            int i = int(std::round(v));
            if (std::fabs(v - i) > 0.01 || i < 0 || i >= names.count()) return QwtText();
            return QwtText(names.at(i));
        }
    private:
        QStringList names;
};

// nice ticks for durations on a log scale
static QList<double>
logDurationTicks(double lo, double hi)
{
    static const double steps[] = { 1, 5, 10, 30, 60, 120, 300, 600, 1200, 1800, 3600, 7200, 10800, 18000, 36000 };
    QList<double> ticks;
    for (double s : steps) if (s >= lo && s <= hi) ticks << s;
    return ticks;
}

struct Theme {
    QColor background, foreground, grid;
};

static Theme
themeFor(bool dark)
{
    if (dark) return { QColor(26, 27, 30), QColor(220, 222, 226), QColor(70, 72, 78) };
    return { QColor(Qt::white), QColor(40, 42, 46), QColor(225, 227, 230) };
}

static void
styleAxis(QwtPlot *plot, int axis, const Theme &theme, const QString &title)
{
    QwtScaleWidget *w = plot->axisWidget(axis);
    QPalette pal = w->palette();
    pal.setColor(QPalette::WindowText, theme.foreground);
    pal.setColor(QPalette::Text, theme.foreground);
    w->setPalette(pal);
    QFont f = w->font();
    f.setPointSizeF(9);
    w->setFont(f);
    if (!title.isEmpty()) {
        QwtText t(title);
        QFont tf = f;
        tf.setBold(true);
        t.setFont(tf);
        t.setColor(theme.foreground);
        plot->setAxisTitle(axis, t);
    }
}

static std::unique_ptr<QwtPlot>
buildPanel(const ChartPanel &panel, const ChartSpec &spec, bool last, const Theme &theme)
{
    std::unique_ptr<QwtPlot> plot(new QwtPlot());
    plot->setAutoReplot(false);
    plot->setAutoFillBackground(true);
    QPalette pal = plot->palette();
    pal.setColor(QPalette::Window, theme.background);
    pal.setColor(QPalette::WindowText, theme.foreground);
    plot->setPalette(pal);
    plot->setCanvasBackground(theme.background);
    plot->plotLayout()->setAlignCanvasToScales(true);
    if (QwtPlotCanvas *canvas = qobject_cast<QwtPlotCanvas *>(plot->canvas())) canvas->setFrameStyle(QFrame::NoFrame);

    if (!panel.title.isEmpty()) {
        QwtText t(panel.title);
        QFont f;
        f.setPointSizeF(10);
        f.setBold(true);
        t.setFont(f);
        t.setColor(theme.foreground);
        plot->setTitle(t);
    }

    QwtPlotGrid *grid = new QwtPlotGrid();
    grid->setPen(QPen(theme.grid, 0, Qt::DotLine));
    grid->attach(plot.get());

    bool anyRight = false;
    double xmin = INFINITY, xmax = -INFINITY;
    for (const ChartSeries &s : panel.series) {
        for (double x : s.x) { if (std::isfinite(x)) { xmin = std::min(xmin, x); xmax = std::max(xmax, x); } }
        if (s.rightAxis) anyRight = true;

        if (s.style == ChartSeries::Bars) {
            QwtPlotBarChart *bars = new QwtPlotBarChart(s.name);
            QVector<QPointF> points;
            for (int i = 0; i < s.x.count() && i < s.y.count(); i++) points << QPointF(s.x[i], s.y[i]);
            bars->setSamples(points);
            QwtColumnSymbol *symbol = new QwtColumnSymbol(QwtColumnSymbol::Box);
            symbol->setFrameStyle(QwtColumnSymbol::Plain);
            symbol->setLineWidth(0);
            symbol->setPalette(QPalette(s.color));
            bars->setSymbol(symbol);
            bars->setLayoutPolicy(QwtPlotAbstractBarChart::AutoAdjustSamples);
            bars->setSpacing(4);
            bars->setAxes(QwtAxis::XBottom, s.rightAxis ? QwtAxis::YRight : QwtAxis::YLeft);
            bars->attach(plot.get());
            continue;
        }

        QwtPlotCurve *curve = new QwtPlotCurve(s.name);
        curve->setRenderHint(QwtPlotItem::RenderAntialiased);
        QPen pen(s.color, s.width);
        if (s.dashed) pen.setStyle(Qt::DashLine);
        curve->setPen(pen);
        if (s.style == ChartSeries::Area) {
            QColor fill = s.color;
            fill.setAlpha(70);
            curve->setBrush(fill);
            curve->setBaseline(0);
        }
        if (s.style == ChartSeries::Dots) {
            curve->setStyle(QwtPlotCurve::NoCurve);
            curve->setSymbol(new QwtSymbol(QwtSymbol::Ellipse, QBrush(s.color), QPen(s.color), QSize(5, 5)));
        }
        curve->setSamples(s.x, s.y);
        curve->setAxes(QwtAxis::XBottom, s.rightAxis ? QwtAxis::YRight : QwtAxis::YLeft);
        curve->attach(plot.get());
    }

    // x axis
    switch (panel.xAxis) {
    case ChartPanel::Duration:
        plot->setAxisScaleDraw(QwtAxis::XBottom, new DurationScaleDraw(false));
        break;
    case ChartPanel::LogDuration: {
        plot->setAxisScaleEngine(QwtAxis::XBottom, new QwtLogScaleEngine());
        plot->setAxisScaleDraw(QwtAxis::XBottom, new DurationScaleDraw(true));
        if (std::isfinite(xmin) && xmax > xmin) {
            xmin = std::max(1.0, xmin);
            QwtScaleDiv div(xmin, xmax);
            div.setTicks(QwtScaleDiv::MajorTick, logDurationTicks(xmin, xmax));
            plot->setAxisScaleDiv(QwtAxis::XBottom, div);
        }
        break;
    }
    case ChartPanel::Date:
        plot->setAxisScaleDraw(QwtAxis::XBottom, new DateScaleDraw());
        break;
    case ChartPanel::Categories: {
        plot->setAxisScaleDraw(QwtAxis::XBottom, new CategoryScaleDraw(panel.categories));
        QList<double> ticks;
        for (int i = 0; i < panel.categories.count(); i++) ticks << i;
        QwtScaleDiv div(-0.5, panel.categories.count() - 0.5);
        div.setTicks(QwtScaleDiv::MajorTick, ticks);
        plot->setAxisScaleDiv(QwtAxis::XBottom, div);
        break;
    }
    case ChartPanel::Plain:
        break;
    }
    if (panel.xAxis == ChartPanel::Duration && std::isfinite(xmin) && xmax > xmin) {
        // round minutes rather than whatever divides the range evenly
        static const double steps[] = { 10, 30, 60, 120, 300, 600, 900, 1200, 1800, 3600, 7200, 10800, 21600 };
        double step = steps[0];
        for (double st : steps) { step = st; if ((xmax - xmin) / st <= 10) break; }
        plot->setAxisScale(QwtAxis::XBottom, xmin, xmax, step);
    } else if (panel.xAxis != ChartPanel::LogDuration && panel.xAxis != ChartPanel::Categories
        && std::isfinite(xmin) && xmax > xmin) {
        plot->setAxisScale(QwtAxis::XBottom, xmin, xmax);
    }

    if (std::isfinite(panel.yMin) || std::isfinite(panel.yMax)) {
        plot->updateAxes();
        QwtInterval iv = plot->axisInterval(QwtAxis::YLeft);
        plot->setAxisScale(QwtAxis::YLeft, std::isfinite(panel.yMin) ? panel.yMin : iv.minValue(),
                           std::isfinite(panel.yMax) ? panel.yMax : iv.maxValue());
    }

    plot->setAxisVisible(QwtAxis::YRight, anyRight);
    styleAxis(plot.get(), QwtAxis::XBottom, theme, last ? spec.xLabel : QString());
    styleAxis(plot.get(), QwtAxis::YLeft, theme, panel.yLabel);
    if (anyRight) styleAxis(plot.get(), QwtAxis::YRight, theme, panel.yRightLabel);

    if (panel.legend && panel.series.count() > 1) {
        QwtLegend *legend = new QwtLegend();
        QPalette lp = legend->palette();
        lp.setColor(QPalette::WindowText, theme.foreground);
        lp.setColor(QPalette::Text, theme.foreground);
        legend->setPalette(lp);
        plot->insertLegend(legend, QwtPlot::TopLegend);
    }

    // scales for the renderer, which paints the plot itself
    plot->updateAxes();
    return plot;
}

static void
paintChart(const ChartSpec &spec, QPainter &painter, const QRectF &area)
{
    Theme theme = themeFor(spec.dark);
    painter.fillRect(area, theme.background);

    QRectF body = area.adjusted(8, 8, -8, -8);
    if (!spec.title.isEmpty()) {
        QFont f;
        f.setPointSizeF(13);
        f.setBold(true);
        painter.setFont(f);
        painter.setPen(theme.foreground);
        QRectF titleRect(body.left(), body.top(), body.width(), 28);
        painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, spec.title);
        body.setTop(body.top() + 32);
    }

    double total = 0;
    for (const ChartPanel &p : spec.panels) total += std::max(0.1, p.stretch);

    double y = body.top();
    QwtPlotRenderer renderer;
    renderer.setDiscardFlag(QwtPlotRenderer::DiscardBackground, false);
    renderer.setDiscardFlag(QwtPlotRenderer::DiscardCanvasFrame, true);
    renderer.setLayoutFlag(QwtPlotRenderer::FrameWithScales, false);

    for (int i = 0; i < spec.panels.count(); i++) {
        const ChartPanel &panel = spec.panels.at(i);
        double h = body.height() * std::max(0.1, panel.stretch) / total;
        std::unique_ptr<QwtPlot> plot = buildPanel(panel, spec, i == spec.panels.count() - 1, theme);
        QRectF rect(body.left(), y, body.width(), h);
        plot->resize(rect.size().toSize());
        renderer.render(plot.get(), &painter, rect);
        y += h;
    }
}

QString
ChartRenderer::mimeType(const QString &format)
{
    if (format == "svg") return "image/svg+xml";
    if (format == "pdf") return "application/pdf";
    return "image/png";
}

QByteArray
ChartRenderer::render(const ChartSpec &spec, const QString &format, QString &error)
{
    error.clear();
    if (spec.panels.isEmpty()) {
        error = "nothing to draw";
        return QByteArray();
    }
    if (spec.size.width() < 100 || spec.size.height() < 100 || spec.size.width() > 10000 || spec.size.height() > 10000) {
        error = "chart size must be between 100 and 10000 pixels";
        return QByteArray();
    }

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QRectF area(QPointF(0, 0), QSizeF(spec.size));

    if (format == "png") {
        QImage image(spec.size, QImage::Format_ARGB32);
        image.fill(themeFor(spec.dark).background);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        paintChart(spec, painter, area);
        painter.end();
        image.save(&buffer, "PNG");

    } else if (format == "svg") {
        QSvgGenerator generator;
        generator.setOutputDevice(&buffer);
        generator.setSize(spec.size);
        generator.setViewBox(area);
        generator.setTitle(spec.title);
        QPainter painter(&generator);
        paintChart(spec, painter, area);
        painter.end();

    } else if (format == "pdf") {
        QPdfWriter writer(&buffer);
        writer.setResolution(96);
        writer.setPageSize(QPageSize(QSizeF(spec.size), QPageSize::Point));
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setTitle(spec.title);
        QPainter painter(&writer);
        QRectF page(QPointF(0, 0), QSizeF(painter.device()->width(), painter.device()->height()));
        paintChart(spec, painter, page);
        painter.end();

    } else {
        error = QString("unknown chart format '%1', use png, svg or pdf").arg(format);
        return QByteArray();
    }
    buffer.close();
    return bytes;
}

} // namespace Headless
