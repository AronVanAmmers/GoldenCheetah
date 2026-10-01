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

#include "ZoneData.h"
#include "ActivitySelection.h"
#include "ActivityJson.h"

#include "Athlete.h"
#include "RideItem.h"
#include "Zones.h"
#include "HrZones.h"
#include "PaceZones.h"
#include "WPrime.h"

#include <climits>
#include <cmath>

namespace Headless {

// the metrics hold at most ten zones of each kind
static const int maxZones = 10;

QStringList zoneTypes() { return { "power", "hr", "pace", "fatigue" }; }

bool
activityZones(Athlete *athlete, RideItem *item, const QString &type, ActivityZones &out, QString &error)
{
    out = ActivityZones();
    out.type = type;
    QDate date = item->dateTime.date();

    if (type == "hr") {
        const HrZones *zones = athlete->hrZones(item->sport);
        int range = zones ? zones->whichRange(date) : -1;
        if (range < 0) { error = "no heart rate zones for the activity date"; return false; }
        out.from = zones->getStartDate(range).toString(Qt::ISODate);
        out.thresholdName = "lthr";
        out.threshold = zones->getLT(range);
        for (int z = 0; z < zones->numZones(range) && z < maxZones; z++) {
            ZoneRow row;
            int lo, hi; double trimp;
            zones->zoneInfo(range, z, row.name, row.description, lo, hi, trimp);
            row.low = lo; row.high = hi; row.open = hi == INT_MAX;
            row.seconds = item->getForSymbol(QString("time_in_zone_H%1").arg(z + 1));
            out.rows << row;
        }

    } else if (type == "pace") {
        // as the GUI: only runs and swims have pace zones
        if (!item->isRun && !item->isSwim) { error = "pace zones are for runs and swims"; return false; }
        const PaceZones *zones = athlete->paceZones(item->isSwim);
        int range = zones ? zones->whichRange(date) : -1;
        if (range < 0) { error = "no pace zones for the activity date"; return false; }
        out.swim = item->isSwim;
        out.from = zones->getStartDate(range).toString(Qt::ISODate);
        out.thresholdName = "cv";
        out.threshold = zones->getCV(range);
        for (int z = 0; z < zones->numZones(range) && z < maxZones; z++) {
            ZoneRow row;
            zones->zoneInfo(range, z, row.name, row.description, row.low, row.high);
            row.open = row.high >= INT_MAX;
            row.seconds = item->getForSymbol(QString("time_in_zone_P%1").arg(z + 1));
            out.rows << row;
        }

    } else if (type == "fatigue") {
        // W' balance zones are fixed fractions of the athlete's W'
        const Zones *zones = athlete->zones(item->sport);
        int range = zones ? zones->whichRange(date) : -1;
        if (range < 0) { error = "no power zones (for W') for the activity date"; return false; }
        int wprime = zones->getWprime(range);
        out.from = zones->getStartDate(range).toString(Qt::ISODate);
        out.thresholdName = "wprime";
        out.threshold = wprime;
        for (int z = 0; z < WPrime::zoneCount(); z++) {
            ZoneRow row;
            row.name = WPrime::zoneName(z);
            row.description = WPrime::zoneDesc(z);
            row.low = WPrime::zoneLo(z, wprime);
            row.high = WPrime::zoneHi(z, wprime);
            row.seconds = item->getForSymbol(QString("wtime_in_zone_L%1").arg(z + 1));
            out.rows << row;
        }

    } else {
        const Zones *zones = athlete->zones(item->sport);
        int range = zones ? zones->whichRange(date) : -1;
        if (range < 0) { error = "no power zones for the activity date"; return false; }
        out.from = zones->getStartDate(range).toString(Qt::ISODate);
        out.thresholdName = "cp";
        out.threshold = zones->getCP(range);
        for (int z = 0; z < zones->numZones(range) && z < maxZones; z++) {
            ZoneRow row;
            int lo, hi;
            zones->zoneInfo(range, z, row.name, row.description, lo, hi);
            row.low = lo; row.high = hi; row.open = hi == INT_MAX;
            row.seconds = item->getForSymbol(QString("time_in_zone_L%1").arg(z + 1));
            out.rows << row;
        }
    }

    // percentages are of the recording time as in the GUI, the time
    // outside the zones (e.g. missing heart rate) is not in any zone
    out.total = item->getForSymbol(type == "fatigue" ? "workout_time" : "time_recording");
    return true;
}

QJsonObject
activityZonesJson(Athlete *athlete, const ActivityZones &zones, bool metricUnits)
{
    const PaceZones *pace = zones.type == "pace" ? athlete->paceZones(zones.swim) : nullptr;

    // pace bounds as the GUI shows them, the speeds alongside
    auto bound = [&](QJsonObject &o, const QString &key, double value) {
        if (!pace) { o.insert(key, jsonNumber(value)); return; }
        o.insert(key, value > 0 ? QJsonValue(pace->kphToPaceString(value, metricUnits)) : QJsonValue());
        o.insert(key + "_kph", jsonNumber(value));
    };

    QJsonObject o;
    o.insert("from", zones.from);
    bound(o, zones.thresholdName, zones.threshold);
    static const QMap<QString, QString> units = { { "power", "watts" }, { "hr", "bpm" }, { "fatigue", "joules" } };
    o.insert("units", pace ? pace->paceUnits(metricUnits) : units.value(zones.type));
    o.insert("total_seconds", jsonNumber(zones.total));    // the percentages are of this

    QJsonArray rows;
    for (const ZoneRow &r : zones.rows) {
        QJsonObject z;
        z.insert("name", r.name);
        z.insert("description", r.description);
        bound(z, "low", r.low);
        if (!r.open) bound(z, "high", r.high);
        z.insert("seconds", jsonNumber(std::round(r.seconds)));
        z.insert("percent", zones.total > 0 ? jsonNumber(std::round(r.seconds / zones.total * 1000) / 10) : QJsonValue(0));
        rows.append(z);
    }
    o.insert("zones", rows);
    return o;
}

} // namespace Headless
