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

#ifndef _GC_ActivitySelection_h
#define _GC_ActivitySelection_h 1

#include "HeadlessCommand.h"

#include <QDate>
#include <QList>

class RideItem;

namespace Headless {

class AthleteSession;

//
// Choosing activities the way the GUI does: a filter expression as typed in
// the filter box (e.g. isRun=0 and Sport = "Bike"), a free text search, a
// named search saved in the GUI, a date range, a sport, or activities named
// one by one. All given criteria must match.
//
struct ActivitySelection {
    QStringList activities;     // ids, see AthleteSession::findActivity
    QString filter;             // DataFilter expression
    QString search;             // free text search
    QString named;              // named search or filter
    QDate from, to;             // inclusive
    QString sport;              // Bike, Run, Swim ... as in the Sport field
    bool planned = false;       // planned instead of actual activities
    int limit = 0;              // most recent n (0 = all)

    bool isEmpty() const;       // nothing chosen, i.e. everything

    // the parameters every selecting command accepts
    static QList<ParamSpec> params(bool positionalActivities = true);

    // read from validated args
    static ActivitySelection fromArgs(const QJsonObject &args);

    // resolve against an open athlete, oldest first. Returns false and sets
    // error (and status) for a bad filter or an unknown activity
    bool resolve(AthleteSession &session, QList<RideItem *> &result, QString &error, Status &status) const;
};

// json for an activity: file, start, sport and the headline numbers
QJsonObject activitySummary(RideItem *item);

// add metric values (by symbol) and metadata fields to an activity object
void addMetrics(QJsonObject &o, RideItem *item, const QStringList &symbols, bool metricUnits = true);
void addMetadata(QJsonObject &o, RideItem *item, const QStringList &fields);

// a metric value for json: no NaN/inf, no 12.300000000001 noise
QJsonValue jsonNumber(double v);

// values of a repeated parameter, each of which may be comma separated
QStringList splitList(const QJsonValue &v);

// local time, as the GUI shows it
QString activityStart(RideItem *item);

// a metric by symbol (average_power) or by the name formulas and the GUI's
// tables use (Average_Power, W'_Work), any case; empty when unknown
QString metricSymbol(const QString &name);

// the lookup behind metricSymbol is rebuilt when the metric count or the
// user metrics change; this forces it, for when a user metric is replaced
void invalidateMetricLookup();
QString metricFormulaName(const QString &symbol);

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

// symbols for names, false and error for the first unknown one
bool resolveMetrics(const QStringList &names, QStringList &symbols, QString &error);

} // namespace Headless

#endif
