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

#include "PerspectiveConfigParser.h"
#include "Utils.h"

const PerspectiveChartConfig::Property *
PerspectiveChartConfig::property(const QString &wanted) const
{
    for (const Property &p : properties) if (p.name == wanted) return &p;
    return nullptr;
}

bool
PerspectiveConfigParser::startDocument()
{
    layouts.clear();
    return true;
}

bool
PerspectiveConfigParser::endElement(const QString&, const QString&, const QString &qName)
{
    if (qName == "layout" && !layouts.isEmpty()) layouts.last().style = style;
    return true;
}

bool
PerspectiveConfigParser::startElement(const QString&, const QString&, const QString &name, const QXmlAttributes &attrs)
{
    if (name == "layout") {

        PerspectiveConfig layout;
        layout.type = type;
        for(int i=0; i<attrs.count(); i++) {
            if (attrs.qName(i) == "style") {
                style = Utils::unprotect(attrs.value(i)).toInt();
            }
            if (attrs.qName(i) == "name") {
                layout.name =  Utils::unprotect(attrs.value(i));
            }
            if (attrs.qName(i) == "expression") {
                layout.expression = Utils::unprotect(attrs.value(i));
            }
            if (attrs.qName(i) == "type") {
                layout.type = Utils::unprotect(attrs.value(i)).toInt();
            }
            if (attrs.qName(i) == "trainswitch") {
                layout.trainswitch = attrs.value(i).toInt();
            }
        }
        layout.style = style;
        layouts << layout;
    }
    else if (name == "chart" && !layouts.isEmpty()) {

        PerspectiveChartConfig chart;
        QString typeStr;

        // get attributes
        for(int i=0; i<attrs.count(); i++) {
            if (attrs.qName(i) == "name") chart.name = Utils::unprotect(attrs.value(i));
            if (attrs.qName(i) == "title") chart.title = Utils::unprotect(attrs.value(i));
            if (attrs.qName(i) == "id")  typeStr = Utils::unprotect(attrs.value(i));
        }
        chart.id = typeStr.toInt();
        layouts.last().charts << chart;
    }
    else if (name == "property" && !layouts.isEmpty() && !layouts.last().charts.isEmpty()) {

        PerspectiveChartConfig::Property property;

        // get attributes
        for(int i=0; i<attrs.count(); i++) {
            if (attrs.qName(i) == "name") property.name = Utils::unprotect(attrs.value(i));
            if (attrs.qName(i) == "value") property.value = Utils::unprotect(attrs.value(i));
            if (attrs.qName(i) == "type")  property.type = Utils::unprotect(attrs.value(i));
        }
        layouts.last().charts.last().properties << property;
    }
    return true;
}
