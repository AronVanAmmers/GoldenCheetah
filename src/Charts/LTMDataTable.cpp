/*
 * Copyright (c) 2010 Mark Liversedge (liversedge@gmail.com)
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
// The table was built and written in one go by LTMWindow::dataTable. It is
// computed here and written by html() and csv(), so the command line can
// have the rows too.
//

#include "LTMDataTable.h"
#include "LTMPlot.h"
#include "LTMSettings.h"
#include "Athlete.h"
#include "Context.h"
#include "Colors.h"
#include "RideCache.h"
#include "RideItem.h"
#include "RideMetric.h"
#include "TimeUtils.h"

#include <QMap>
#include <float.h>

// for storing curve data without using a curve
class TableCurveData {
    public:
        TableCurveData() { n=0; x.resize(0); y.resize(0); }
        QVector<double> x,y;
        int n;
};

int
LTMDataTable::groupForDate(const LTMSettings &settings, QDate date)
{
    switch(settings.groupBy) {
    case LTM_WEEK:
        {
        // must start from 1 not zero!
        return 1 + ((date.toJulianDay() - settings.start.date().toJulianDay()) / 7);
        }
    case LTM_MONTH: return (date.year()*12) + date.month();
    case LTM_YEAR:  return date.year();
    case LTM_DAY:
    default:
        return date.toJulianDay();

    }
}

LTMDataTable::LTMDataTable(Context *context, LTMPlot *ltmPlot, LTMSettings &settings) : any(false)
{
    // truncate date range to the actual data when not set to any date
    if (context->athlete->rideCache->rides().count()) {

        QDateTime first = context->athlete->rideCache->rides().first()->dateTime;
        QDateTime last = context->athlete->rideCache->rides().last()->dateTime;

        // end
        if (settings.end == QDateTime() || settings.end.date() > QDate::currentDate().addYears(40))
                settings.end = last;

        // start
        if (settings.start == QDateTime() || settings.start.date() < QDate::currentDate().addYears(-40))
            settings.start = first;
    }

    title = settings.title;
    groupBy = settings.groupBy;
    curveCount = settings.metrics.count();

    //
    // STEP1: AGGREGATE DATA INTO GROUPBY FOR EACH METRIC
    //        This is performed by reusing the existing code in
    //        LTMPlot for creating curve data, but storing it
    //        in columns and forceing zero values
    QList<TableCurveData> columnData;
    bool first=true;
    int rowCount = 0;
    bool firstXvalue=true;
    double lowestFirstXvalue = DBL_MAX;
    double highestFirstXvalue = 0.0;

    // create curve data for each metric detail to iterate over
    foreach(MetricDetail metricDetail, settings.metrics) {
        TableCurveData add;

        ltmPlot->settings=&settings; // for stack mode ltmPlot isn't set
        if (settings.groupBy != LTM_TOD)
            ltmPlot->createCurveData(context, &settings, metricDetail, add.x, add.y, add.n, true);
        else
            ltmPlot->createTODCurveData(context, &settings, metricDetail, add.x, add.y, add.n, true);

        // adjust to avoid empty chart when there is only 1 group
        if (settings.groupBy != LTM_TOD) add.n++;

        columnData << add;

        // check if "x" value of all metrics is the same for all colums and find
        // the lowest "x" value and highest "x" value to which all columns need to be aligned
        if (add.n > 0) {
            if (firstXvalue) {
                lowestFirstXvalue = highestFirstXvalue = add.x[0];
                firstXvalue = false;
            } else {
                if (add.x[0] < lowestFirstXvalue) {
                    lowestFirstXvalue = add.x[0];
                }
                if (add.x[0] > highestFirstXvalue) {
                    highestFirstXvalue = add.x[0];
                }
            }
        }

        // truncate to shortest set of rows available as
        // we dont pad with zeroes in the data table
        if (first) rowCount=add.n;
        else if (add.n < rowCount) rowCount=add.n;
        first=false;
    }

    // align the starting X values of all columns using the
    // lowest xValue and highest xValue as borders
    // for columns which have data at all - and if there is something to adjust
    if (!firstXvalue && lowestFirstXvalue != highestFirstXvalue) {
        for (int i = 0; i< columnData.count(); i++) {
            if (columnData[i].n > 0) {
                // Prepend on vector is prohibitively expensive since requires
                // full vector copy for each prepend. Much faster to convert
                // to Qlist, do our business, then convert back.
                QList<double> tx = columnData[i].x.toList();
                QList<double> ty = columnData[i].y.toList();

                double xValue = columnData[i].x[0];
                while (xValue > lowestFirstXvalue) {
                    xValue--;
                    tx.prepend(xValue);
                    ty.prepend(0.0);
                }

                columnData[i].x = tx.toVector();
                columnData[i].y = ty.toVector();
                columnData[i].n += tx.size();
            }
        }
        // adjust number of visible rows in table
        rowCount += qRound(highestFirstXvalue-lowestFirstXvalue);
    }

    //
    // STEP 2: THE ROWS FROM THE AGGREGATED DATA
    //         But note there will be no data if there are no curves of if there
    //         is no date range selected of no data anyway!
    //
    if (!rowCount) return;
    any = true;

    // formatting ...
    LTMScaleDraw lsd(settings.start, groupForDate(settings, settings.start.date()), settings.groupBy);

    dateHeading = (settings.groupBy == LTM_TOD) ? tr("Time of Day") : tr("Date");

    QList<QVector<double> > hdatas;

    // highlight
    for (int a=0; a < settings.metrics.count(); a++) {
        MetricDetail metricDetail = settings.metrics[a];

        // highlight lowest / top N values
        if (metricDetail.lowestN > 0 || metricDetail.topN > 0) {
            QMap<double, int> sortedList;

            // copy the yvalues, retaining the offset
            for(int i=0; i<columnData[a].y.count(); i++) {
                // pmc metrics we highlight TROUGHS
                if (metricDetail.type == METRIC_STRESS || metricDetail.type == METRIC_PM) {
                    if (i && i < (columnData[a].y.count()-1) // not at start/end
                        && ((columnData[a].y[i-1] > columnData[a].y[i] && columnData[a].y[i+1] > columnData[a].y[i]) || // is a trough
                            (columnData[a].y[i-1] < columnData[a].y[i] && columnData[a].y[i+1] < columnData[a].y[i])))  // is a peak
                        sortedList.insert(columnData[a].y[i], i);
                } else
                    sortedList.insert(columnData[a].y[i], i);
            }

            // copy the top N values
            QVector<double> hdata;
            hdata.resize(metricDetail.topN + metricDetail.lowestN);


            // QMap orders the list so start at the top and work
            // backwards for topN
            int counter = 0;
            QMapIterator<double, int> i(sortedList);
            if (metricDetail.topN) {
                i.toBack();
                while (i.hasPrevious() && counter < metricDetail.topN) {
                    i.previous();
                    hdata[counter] = i.value();
                    counter++;
                }
            }

            if (metricDetail.lowestN) {
                i.toFront();
                counter = 0; // and forwards for bottomN
                while (i.hasNext() && counter < metricDetail.lowestN) {
                    i.next();
                    hdata[metricDetail.topN + counter] = i.value();
                    counter++;
                }
            }
            hdatas.append(hdata);
        } else {
            // add an empty vector to maintain alignment with the columns
            QVector<double> hdata;
            hdatas.append(hdata);
        }
    }

    // metric name and units
    for (int i=0; i < settings.metrics.count(); i++) {

        Column column;
        QString name = settings.metrics[i].uname;

        if (name == "Coggan Acute Training Load" || name == tr("Coggan Acute Training Load")) name = "ATL";
        if (name == "Coggan Chronic Training Load" || name == tr("Coggan Chronic Training Load")) name = "CTL";
        if (name == "Coggan Training Stress Balance" || name == tr("Coggan Training Stress Balance")) name = "TSB";
        column.name = name;

        QString units = settings.metrics[i].uunits;
        if (units == "seconds" || units == tr("seconds")) units = tr("hours");
        if (units == settings.metrics[i].uname) units = "";
        column.units = units;

        column.color = settings.metrics[i].penColor;
        columns << column;
    }

    for(int row=0; row<rowCount; row++) {

        // in day mode we don't list all the zeroes .. its too many!
        bool nonzero = false;
        if (settings.groupBy == LTM_DAY) {

            // nonzeros?
            for(int j=0; j<columnData.count(); j++)
                if (int(columnData[j].y[row])) nonzero = true;

            // skip all zeroes if day mode
            if (nonzero == false) continue;
        }

        Row add;
        add.index = row;

        // First column, date / month year etc
        add.label = lsd.label(columnData[0].x[row]+0.5).text().replace("\n", " ");
        add.date = (settings.groupBy == LTM_ALL || settings.groupBy == LTM_TOD) ? add.label : lsd.toDate(columnData[0].x[row]+0.5).toString(Qt::ISODate);

        // Remaining columns - each metric value
        for(int j=0; j<columnData.count(); j++) {

            // now format the actual value....
            QString valueString;
            double value = columnData[j].y[row];

            // Format minutes in sexagesimal format
            if (LTMPlot::isMinutes(settings.metrics[j].uunits)) {
                valueString = time_to_string(value * 60, true);
            } else {
                int precision = 1;
                const RideMetric *m = settings.metrics[j].metric;
                if (m != NULL) {

                    // we have a metric so lets be precise ...
                    precision = m->precision();

                    // handle precision of 1 for seconds converted to hours
                    if (settings.metrics[j].uunits == "seconds" || settings.metrics[j].uunits == tr("seconds")) precision = 1;
                }
                valueString.setNum(value, 'f', precision);
            }

            add.values << value;
            add.text << valueString;
            add.highlighted << hdatas.at(j).contains(row);
        }
        rows << add;
    }
}

QString
LTMDataTable::html() const
{
    // now set to new (avoids a weird crash)
    QString summary;

    QColor bgColor = GColor(CTRENDPLOTBACKGROUND);
    QColor altColor = GCColor::alternateColor(bgColor);

    // html page prettified with a title
    summary = GCColor::css();
    summary += "<center>";

    // device summary for ride summary, otherwise how many activities?
    summary += "<p><h3>" + title + tr(" grouped by ");

    switch (groupBy) {
    case LTM_DAY :
        summary += tr("day");
        break;
    case LTM_WEEK :
        summary += tr("week");
        break;
    case LTM_MONTH :
        summary += tr("month");
        break;
    case LTM_YEAR :
        summary += tr("year");
        break;
    case LTM_TOD :
        summary += tr("time of day");
        break;
    case LTM_ALL :
        summary += tr("All");
        break;
    }
    summary += "</h3><p>";

    if (any) {

        // table and headings 50% for 1 metric, 70% for 2 metrics, 90% for 3 metrics or more
        QString tableStart = "<table border=0 cellspacing=3 width=\"%1%%\"><tr><td align=\"center\" valigne=\"top\"><b>%2</b></td>";
        tableStart = tableStart.arg(curveCount >= 3 ? 90 : (30 + (curveCount * 20))).arg(dateHeading);

        summary += tableStart;

        QList<QString> fontcolors;
        for (const Column &column : columns) {
            int brightness = column.color.red() *0.299 + column.color.green()*0.587 + column.color.blue()*0.114;
            fontcolors.append( brightness > 128 ? "#000" : "#fff" );
        }

        // metric name
        for (int i=0; i < columns.count(); i++) {

            QString metricSummary = "<td align=\"center\" style=\"font-weight:bold;background-color:%2;color:%3\" valign=\"top\">%1</td>";
            metricSummary = metricSummary.arg(columns[i].name);
            QString bcolor = columns[i].color.lighter(80).name();
            metricSummary = metricSummary.arg(bcolor);
            metricSummary = metricSummary.arg(fontcolors.at(i));

            summary += metricSummary;
        }

        // html table and units on next line
        summary += "</tr><tr><td></td>";

        // units
        for (int i=0; i < columns.count(); i++) {
            QString metricSummary = "<td align=\"center\" style=\"font-weight:bold;background-color:%2;color:%3\" valign=\"top\">"
                    "%1</td>";
            QString units = columns[i].units;
            QString bcolor = columns[i].color.lighter(80).name();

            metricSummary = metricSummary.arg(units != "" ? QString("(%1)").arg(units) : "");
            metricSummary = metricSummary.arg(bcolor);
            metricSummary = metricSummary.arg(fontcolors.at(i));

            summary += metricSummary;
        }
        summary += "</tr>";

        for (const Row &row : rows) {

            QString rowSummary;

            // alternating colors on html output
            if (row.index%2) rowSummary += "<tr bgcolor='" + altColor.name() + "'>";
            else rowSummary += "<tr>";

            // First column, date / month year etc
            rowSummary += QString("<td align=\"center\" valign=\"top\">%1</td>").arg(row.label);

            // Remaining columns - each metric value
            for(int j=0; j<row.text.count(); j++) {

                QString metricSummary = "<td align=\"center\" style=\"%2\" valign=\"top\">%1</td>";
                metricSummary = metricSummary.arg(row.text.at(j));

                if (row.highlighted.at(j)) {
                    QString c = QString("background-color:%1;color:%2").arg(columns[j].color.name()).arg(fontcolors.at(j));
                    metricSummary = metricSummary.arg(c);
                } else
                    metricSummary = metricSummary.arg("");

                rowSummary += metricSummary;
            }

            // ok, this row is done
            rowSummary += "</tr>";

            summary += rowSummary;
        }

        // close table on html page
        summary += "</table>";
    }

    // all done !
    summary += "</center>";

    return summary;
}

QString
LTMDataTable::csv() const
{
    QString summary;
    if (!any) return summary;

    summary += dateHeading;

    // metric name
    for (const Column &column : columns) summary += QString(", %1").arg(column.name);

    // end of heading for CSV
    summary += "\n";

    for (const Row &row : rows) {

        // First column, date / month year etc
        QString rowSummary = row.date;

        // Remaining columns - each metric value
        for (const QString &value : row.text) rowSummary += QString(", %1").arg(value);

        rowSummary += "\n"; // csv newline
        summary += rowSummary;
    }

    return summary;
}
