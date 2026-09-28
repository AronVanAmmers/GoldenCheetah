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

#ifndef _GC_ZoneData_h
#define _GC_ZoneData_h 1

// time in zones for an activity, shared by activity show and the zones chart

#include "HeadlessCommand.h"

class Athlete;
class RideItem;

namespace Headless {

struct ZoneRow {
    QString name, description;
    double low = 0, high = 0;   // watts, bpm, km/h or joules of W' left
    bool open = false;          // the top zone has no upper bound
    double seconds = 0;
};

struct ActivityZones {
    QString type;               // power, hr, pace or fatigue (W' balance)
    QString from;               // first day of the zone range used
    QString thresholdName;      // cp, lthr, cv or wprime
    double threshold = 0;
    bool swim = false;          // pace zones for swimming
    QList<ZoneRow> rows;
    double total = 0;           // seconds the percentages are of
};

QStringList zoneTypes();        // power, hr, pace, fatigue

// the athlete's zones for the activity's sport and date with the time spent
// in each, as the overview's zone tables; false and error when there are none
bool activityZones(Athlete *athlete, RideItem *item, const QString &type, ActivityZones &out, QString &error);

// the zone table as json: bounds as the GUI shows them (pace as mm:ss)
QJsonObject activityZonesJson(Athlete *athlete, const ActivityZones &zones, bool metricUnits);

} // namespace Headless

#endif
