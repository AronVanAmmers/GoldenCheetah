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

#include "SeasonRange.h"
#include "AthleteSession.h"

#include "Athlete.h"
#include "Season.h"
#include "Seasons.h"

#include <QUuid>

namespace Headless {

static bool
sameId(const QUuid &id, const QString &key)
{
    QUuid wanted(key.trimmed());
    return !wanted.isNull() && wanted == id;
}

QList<SeasonMatch>
findSeasons(const QList<Season> &seasons, const QString &key, bool withPhases)
{
    QList<SeasonMatch> found;
    QString k = key.trimmed();
    for (int i = 0; i < seasons.count(); i++) {
        const Season &s = seasons.at(i);
        if (sameId(s.id(), k) || s.getName().compare(k, Qt::CaseInsensitive) == 0) found << SeasonMatch{ i, -1 };
        if (!withPhases) continue;
        for (int j = 0; j < s.phases.count(); j++) {
            const Phase &p = s.phases.at(j);
            if (sameId(p.id(), k) || (s.getName() + "/" + p.getName()).compare(k, Qt::CaseInsensitive) == 0)
                found << SeasonMatch{ i, j };
        }
    }
    return found;
}

bool
findOneSeason(const QList<Season> &seasons, const QString &key, bool withPhases,
              SeasonMatch &match, QString &error, Status &status)
{
    QList<SeasonMatch> found = findSeasons(seasons, key, withPhases);
    if (found.count() == 1) {
        match = found.first();
        return true;
    }
    if (found.isEmpty()) {
        error = QString("no season called '%1', see 'season list'").arg(key);
        status = Status::NotFound;
        return false;
    }
    QStringList names;
    for (const SeasonMatch &m : found) {
        const Season &s = seasons.at(m.season);
        if (m.phase < 0) names << QString("%1 %2").arg(s.getName(), s.id().toString());
        else names << QString("%1/%2 %3").arg(s.getName(), s.phases.at(m.phase).getName(), s.phases.at(m.phase).id().toString());
    }
    error = QString("'%1' matches more than one season, give the id: %2").arg(key, names.join(", "));
    status = Status::Usage;
    return false;
}

bool
seasonRange(AthleteSession &session, const QString &name, DateRange &out, QString &error, Status *status)
{
    Status st = Status::Ok;
    SeasonMatch match;
    const QList<Season> &seasons = session.athlete()->seasons->seasons;
    if (!findOneSeason(seasons, name, true, match, error, st)) {
        if (status) *status = st;
        return false;
    }
    const Season &season = seasons.at(match.season);
    if (match.phase >= 0) {
        const Phase &phase = season.phases.at(match.phase);
        out = DateRange(phase.getStart(), phase.getEnd(), season.getName() + "/" + phase.getName());
        out.id = phase.id();
    } else {
        out = DateRange(season.getStart(), season.getEnd(), season.getName());
        out.id = season.id();
    }
    if (status) *status = Status::Ok;
    return true;
}

ParamSpec
seasonParam()
{
    return ParamSpec("season", ParamType::String,
                     "the dates of this season, phase (Season/Phase) or date range such as 'This Year', by name or id "
                     "(see 'season list'), instead of --from and --to");
}

bool
dateRangeArgs(AthleteSession &session, const QJsonObject &args, QDate &from, QDate &to,
              QString &error, Status &status)
{
    from = args.contains("from") ? QDate::fromString(args.value("from").toString(), Qt::ISODate) : QDate();
    to = args.contains("to") ? QDate::fromString(args.value("to").toString(), Qt::ISODate) : QDate();
    if (!args.contains("season")) return true;

    if (args.contains("from") || args.contains("to")) {
        error = "--season stands for --from and --to: give one or the other";
        status = Status::Usage;
        return false;
    }
    DateRange range;
    if (!seasonRange(session, args.value("season").toString(), range, error, &status)) return false;
    from = range.from;
    to = range.to;
    return true;
}

} // namespace Headless
