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

#include "PlanAdherenceData.h"

#include "RideCache.h"
#include "RideItem.h"

#include <algorithm>
#include <cstdlib>


void
planAdherence
(RideCache *rideCache, const QDate &firstVisible, const QDate &lastVisible, const QDate &today,
 const std::function<bool(RideItem*)> &include,
 QList<PlanAdherenceRow> &rows, PlanAdherenceStatistics &statistics, PlanAdherenceOffsetRange &offsetRange)
{
    rows.clear();
    statistics = PlanAdherenceStatistics();
    offsetRange = PlanAdherenceOffsetRange();
    for (RideItem *rideItem : rideCache->rides()) {
        if (   rideItem == nullptr
            || (! rideItem->planned && rideItem->hasLinkedActivity())
            || (include && ! include(rideItem))) {
            continue;
        }
        QDate rideDate = rideItem->dateTime.date();
        QString originalDateString = rideItem->getText("Original Date", "");
        QDate originalDate(rideDate);
        if (! originalDateString.isEmpty()) {
            originalDate = QDate::fromString(originalDateString, "yyyy/MM/dd");
            if (! originalDate.isValid()) {
                originalDate = rideDate;
            }
        }
        if (   (firstVisible.isValid() && originalDate < firstVisible)
            || (lastVisible.isValid() && originalDate > lastVisible)) {
            continue;
        }

        PlanAdherenceRow row;
        row.rideItem = rideItem;
        row.date = originalDate;

        if (rideItem->planned && originalDate != rideDate) {
            row.shiftOffset = originalDate.daysTo(rideDate);
            offsetRange.min = std::min(offsetRange.min, row.shiftOffset.value());
            offsetRange.max = std::max(offsetRange.max, row.shiftOffset.value());
        } else {
            row.shiftOffset.reset();
        }

        RideItem *linkedItem = nullptr;
        if (! rideItem->getLinkedFileName().isEmpty()) {
            linkedItem = rideCache->getRide(rideItem->getLinkedFileName());
        }
        if (rideItem->planned && linkedItem != nullptr) {
            row.linkedItem = linkedItem;
            row.actualOffset = originalDate.daysTo(linkedItem->dateTime.date());
            offsetRange.min = std::min(offsetRange.min, row.actualOffset.value());
            offsetRange.max = std::max(offsetRange.max, row.actualOffset.value());
        } else {
            row.actualOffset.reset();
        }

        rows << row;

        ++statistics.totalAbs;
        if (rideItem->planned) {
            ++statistics.plannedAbs;
            if (row.shiftOffset != std::nullopt) {
                ++statistics.shiftedAbs;
                statistics.totalShiftDaysAbs += std::abs(row.shiftOffset.value());
            }
            if (row.actualOffset != std::nullopt) {
                if (row.actualOffset.value() == 0) {
                    ++statistics.onTimeAbs;
                }
            } else if (row.date < today) {
                ++statistics.missedAbs;
            }
        } else {
            ++statistics.unplannedAbs;
        }
    }
    std::sort(rows.begin(), rows.end(), [](const PlanAdherenceRow &a, const PlanAdherenceRow &b) {
        return a.date < b.date;
    });
    if (statistics.totalAbs > 0) {
        statistics.plannedRel = 100.0 * statistics.plannedAbs / statistics.totalAbs;
        statistics.unplannedRel = 100.0 * statistics.unplannedAbs / statistics.totalAbs;
    }
    if (statistics.plannedAbs > 0) {
        statistics.onTimeRel = 100.0 * statistics.onTimeAbs / statistics.plannedAbs;
        statistics.missedRel = 100.0 * statistics.missedAbs / statistics.plannedAbs;
        statistics.shiftedRel = 100.0 * statistics.shiftedAbs / statistics.plannedAbs;
    }
    if (statistics.shiftedAbs > 0) {
        statistics.avgShift = statistics.totalShiftDaysAbs / static_cast<float>(statistics.shiftedAbs);
    }
}
