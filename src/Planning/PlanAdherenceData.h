/*
 * Copyright (c) 2026 Joachim Kohlhammer (joachim.kohlhammer@gmx.de)
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

#ifndef PLANADHERENCEDATA_H
#define PLANADHERENCEDATA_H

#include <QDate>
#include <QList>
#include <functional>
#include <optional>

class RideItem;
class RideCache;

//
// What the Plan Adherence chart shows, without the drawing: each planned
// activity of the period by the day it was planned for, how far it was moved
// and when it was done (its linked activity), each completed activity that
// wasn't planned, and the totals. The chart (PlanAdherenceWindow) and the
// headless 'plan adherence' command both use it.
//

struct PlanAdherenceStatistics {
    int totalAbs = 0;
    int plannedAbs = 0;
    float plannedRel = 0;
    int onTimeAbs = 0;
    float onTimeRel = 0;
    int shiftedAbs = 0;
    float shiftedRel = 0;
    int missedAbs = 0;
    float missedRel = 0;
    float avgShift = 0;
    int unplannedAbs = 0;
    float unplannedRel = 0;
    int totalShiftDaysAbs = 0;
};


struct PlanAdherenceOffsetRange {
    qint64 min = -1;
    qint64 max = 1;
};


struct PlanAdherenceRow {
    RideItem *rideItem = nullptr;           // a planned activity, or a completed one that wasn't planned
    RideItem *linkedItem = nullptr;         // the completed activity a planned one is linked to
    QDate date;                             // the day it was (first) planned for
    std::optional<qint64> shiftOffset;      // days it was moved, when it was
    std::optional<qint64> actualOffset;     // days from planned to done, when it was done
};


// the rows from firstDay to lastDay (either may be null: open), by date.
// include leaves activities out (the chart's filters); today decides what
// was missed
void planAdherence(RideCache *rideCache, const QDate &firstDay, const QDate &lastDay, const QDate &today,
                   const std::function<bool(RideItem*)> &include,
                   QList<PlanAdherenceRow> &rows, PlanAdherenceStatistics &statistics,
                   PlanAdherenceOffsetRange &offsetRange);

#endif
