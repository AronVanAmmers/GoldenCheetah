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

#ifndef _GC_SeasonRange_h
#define _GC_SeasonRange_h 1

//
// Seasons as date ranges: finding a season (or a phase) of the athlete by
// name or id, and the --season option that stands for its dates wherever a
// command takes --from and --to.
//

#include "HeadlessCommand.h"
#include "TimeUtils.h"

#include <QDate>
#include <QList>

class Season;

namespace Headless {

class AthleteSession;

// a season, or one of its phases (phase -1 for the season itself), by
// index into Seasons::seasons
struct SeasonMatch {
    int season = -1;
    int phase = -1;
};

// Seasons and phases called key: a season by its name (case-insensitive)
// or id ({...}, braces optional), a phase by its id or "Season/Phase" as
// the sidebar names it. Phases are only looked at with withPhases.
QList<SeasonMatch> findSeasons(const QList<Season> &seasons, const QString &key, bool withPhases);

// exactly one match, or false with error and status set: NotFound when
// nothing matches, Usage when several do (they are listed with their ids)
bool findOneSeason(const QList<Season> &seasons, const QString &key, bool withPhases,
                   SeasonMatch &match, QString &error, Status &status);

// The dates of a season or phase (see findSeasons), resolved as of today,
// the way the Trends sidebar resolves it when the range is chosen. out
// gets from, to, the name ("Season/Phase" for a phase) and the id.
bool seasonRange(AthleteSession &session, const QString &name, DateRange &out, QString &error,
                 Status *status = nullptr);

// the --season option
ParamSpec seasonParam();

// --from and --to from args, or --season's dates instead; the dates that
// are not given are left invalid. --season with --from or --to is refused.
bool dateRangeArgs(AthleteSession &session, const QJsonObject &args, QDate &from, QDate &to,
                   QString &error, Status &status);

} // namespace Headless

#endif
