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

#ifndef _GC_PerspectiveConfigParser_h
#define _GC_PerspectiveConfigParser_h 1

#include <QString>
#include <QList>
#include <QXmlDefaultHandler>

//
// What a view's layouts file (athletehome/config/xxx-perspectives.xml)
// holds, read without making any windows: each layout (perspective) with
// its charts and their saved properties, as text. ViewParser makes the
// windows from it; the command line reads the overview tiles from it.
//
struct PerspectiveChartConfig {

    struct Property {
        QString name, type, value;  // value as saved, unprotected
    };

    QString name, title;
    int id = 0;                     // GcWinID
    QList<Property> properties;

    const Property *property(const QString &name) const;
};

struct PerspectiveConfig {
    QString name = "General";
    QString expression;
    int type = 0;                   // the view type it is for
    int trainswitch = 0;
    int style = 2;                  // the style in effect when the layout was read
    QList<PerspectiveChartConfig> charts;
};

class PerspectiveConfigParser : public QXmlDefaultHandler
{
    public:

        // type: the view type a layout gets when it doesn't say
        explicit PerspectiveConfigParser(int type) : style(2), type(type) {}

        // the results!
        QList<PerspectiveConfig> layouts;
        int style;

        // unmarshall
        bool startDocument();
        bool endElement(const QString&, const QString&, const QString &qName);
        bool startElement(const QString&, const QString&, const QString &name, const QXmlAttributes &attrs);

    protected:

        int type;
};

#endif
