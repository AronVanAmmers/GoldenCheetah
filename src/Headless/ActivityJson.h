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

#ifndef _GC_ActivityJson_h
#define _GC_ActivityJson_h 1

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonValue>

class RideItem;

namespace Headless {

//
// Activities as the commands report them in JSON.
//

// json for an activity: file, start, sport and the headline numbers
QJsonObject activitySummary(RideItem *item);

// add metric values (by symbol) and metadata fields to an activity object
void addMetrics(QJsonObject &o, RideItem *item, const QStringList &symbols, bool metricUnits = true);
void addMetadata(QJsonObject &o, RideItem *item, const QStringList &fields);

// a metric value for json: no NaN/inf, no 12.300000000001 noise
QJsonValue jsonNumber(double v);

// local time, as the GUI shows it
QString activityStart(RideItem *item);


} // namespace Headless

#endif
