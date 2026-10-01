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
    bool all = false;           // --all, for the commands that change activities

    bool isEmpty() const;       // nothing chosen, i.e. everything

    // commands that change activities change none until some are chosen
    // or --all is given: the usage error, or empty when that is the case
    QString requireExplicit() const;

    // the parameters every selecting command accepts
    static QList<ParamSpec> params(bool positionalActivities = true);

    // read from validated args
    static ActivitySelection fromArgs(const QJsonObject &args);

    // resolve against an open athlete, oldest first. Returns false and sets
    // error (and status) for a bad filter or an unknown activity
    bool resolve(AthleteSession &session, QList<RideItem *> &result, QString &error, Status &status) const;
};

// values of a repeated parameter, each of which may be comma separated
QStringList splitList(const QJsonValue &v);

// NAME=VALUE pairs, as --set takes them; false with bad set to the first
// one that has no name
bool parseAssignments(const QJsonValue &list, QList<QPair<QString, QString>> &pairs, QString &bad);

} // namespace Headless

#endif
