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

#ifndef _GC_LTMDataTable_h
#define _GC_LTMDataTable_h 1

#include <QCoreApplication>
#include <QColor>
#include <QDate>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

class Context;
class LTMPlot;
class LTMSettings;

//
// The data table of a Trends chart: what its Data Table view shows (html())
// and "Export Chart Data..." writes (csv()). The values come from the plot's
// own curve data, grouped as the chart groups. The window and the command
// line both use it.
//
class LTMDataTable
{
    Q_DECLARE_TR_FUNCTIONS(LTMWindow)

    public:

        // builds the table. A chart with no dates set gets those of the
        // first and last activity, as the window's settings do
        LTMDataTable(Context *context, LTMPlot *plot, LTMSettings &settings);

        struct Column {
            QString name;           // the curve's name, ATL, CTL or TSB for Coggan's
            QString units;          // as the heading shows them: hours for seconds
            QColor color;           // the curve's colour
        };

        struct Row {
            int index;              // in the groups, before day mode drops empty days
            QString label;          // as the table shows it: "Jan 3 2024", "Week ..."
            QString date;           // as the export writes it: an ISO date, or the label for all or time of day
            QVector<double> values; // one per column
            QStringList text;       // the values as shown
            QVector<bool> highlighted; // a top or lowest N value
        };

        QString dateHeading;        // Date, or Time of Day
        QList<Column> columns;
        QList<Row> rows;            // the rows listed: in day mode days that are all zero are left out

        QString html() const;       // the Data Table view
        QString csv() const;        // Export Chart Data

        // the group a date falls in, counted as the table and the window's
        // popup count them
        static int groupForDate(const LTMSettings &settings, QDate date);

    private:
        QString title;
        int groupBy;
        int curveCount;
        bool any;                   // any groups at all (a table is drawn)
};

#endif
