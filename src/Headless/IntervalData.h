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

#ifndef _GC_IntervalData_h
#define _GC_IntervalData_h 1

#include <QString>
#include <QStringList>

#include "RideFile.h"

class RideItem;

namespace Headless {

//
// The intervals of an activity: their types and how many there are of each.
//

// an interval type as interval list --type takes it (user, effort ...)
QString intervalTypeKey(int type);

// how many intervals of each kind an activity has, including zeros.
// recorded laps are from the device, user intervals were marked in
// GoldenCheetah, discovered efforts are the ones it found (efforts,
// peaks, climbs, segments). The entire activity is none of these.
struct IntervalCensus {
    int recordedLaps = 0;
    int userIntervals = 0;
    int discoveredEfforts = 0;
};

IntervalCensus intervalCensus(RideItem *item);
QString intervalCensusLine(const IntervalCensus &census);

// a type by its key or by the group title the GUI shows (EFFORTS, PEAK POWER)
bool intervalTypeFromName(const QString &name, RideFileInterval::IntervalType &type);

// the keys, as interval list --type takes them
QStringList intervalTypeNames();


} // namespace Headless

#endif
