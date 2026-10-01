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

#include "IntervalData.h"

#include "RideItem.h"
#include "RideFile.h"
#include "IntervalItem.h"

namespace Headless {

// stable names for the interval types, the GUI's group titles are translated
static const QList<QPair<QString, RideFileInterval::IntervalType>> &
intervalTypes()
{
    static const QList<QPair<QString, RideFileInterval::IntervalType>> types = {
        { "user", RideFileInterval::USER }, { "all", RideFileInterval::ALL },
        { "device", RideFileInterval::DEVICE }, { "peakpower", RideFileInterval::PEAKPOWER },
        { "peakpace", RideFileInterval::PEAKPACE }, { "effort", RideFileInterval::EFFORT },
        { "route", RideFileInterval::ROUTE }, { "climb", RideFileInterval::CLIMB }
    };
    return types;
}

QString
intervalTypeKey(int type)
{
    for (const auto &t : intervalTypes()) if (t.second == type) return t.first;
    return QString();
}

IntervalCensus
intervalCensus(RideItem *item)
{
    IntervalCensus census;
    if (!item) return census;
    for (IntervalItem *interval : item->intervals()) {
        switch (interval->type) {
        case RideFileInterval::DEVICE: census.recordedLaps++; break;
        case RideFileInterval::USER: census.userIntervals++; break;
        case RideFileInterval::EFFORT:
        case RideFileInterval::PEAKPOWER:
        case RideFileInterval::PEAKPACE:
        case RideFileInterval::CLIMB:
        case RideFileInterval::ROUTE: census.discoveredEfforts++; break;
        default: break; // the entire activity
        }
    }
    return census;
}

QString
intervalCensusLine(const IntervalCensus &census)
{
    return QString("recorded laps: %1, user intervals: %2, discovered efforts: %3")
        .arg(census.recordedLaps).arg(census.userIntervals).arg(census.discoveredEfforts);
}

// a type by its key or by the group title the GUI shows (EFFORTS, PEAK POWER)
bool
intervalTypeFromName(const QString &name, RideFileInterval::IntervalType &type)
{
    QString n = QString(name).remove(' ').toLower();
    for (const auto &t : intervalTypes()) {
        QString title = RideFileInterval::typeDescription(t.second).remove(' ').toLower();
        if (n == t.first || n == title || n + "s" == title) { type = t.second; return true; }
    }
    return false;
}

QStringList
intervalTypeNames()
{
    QStringList names;
    for (const auto &t : intervalTypes()) names << t.first;
    return names;
}

} // namespace Headless
