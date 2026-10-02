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

#ifndef _GC_SeasonDefinition_h
#define _GC_SeasonDefinition_h 1

//
// A season's date range as the Edit Date Range dialog (EditSeasonDialog)
// shows it: a start that is a date, "N weeks/months/years ago" or a length
// before the end, and an end that is a date, "N ... ago", a length after the
// start or year to date. The command line flags map onto this one to one,
// and it is applied through the Season setters the dialog uses.
//
// Depends on Season only, so it is unit tested without an athlete.
//

#include "Season.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace Headless {

struct SeasonBound {
    enum Kind { None, Absolute, Relative, Duration, Ytd };

    Kind kind = None;
    QDate date;                 // Absolute
    int ago = 0;                // Relative: this many units before today
    SeasonOffset::SeasonOffsetType unit = SeasonOffset::week;
};

struct SeasonDefinition {
    SeasonBound start, end;
    SeasonLength length;        // used by the side that is a Duration

    // as the dialog reads a season (EditSeasonDialog::transferSeasonToUI)
    static SeasonDefinition of(const Season &season);

    // as the dialog writes it (transferUIToSeason): resets the range first
    void applyTo(Season &season) const;

    // what the dialog would not allow for a season of this type, with or
    // without phases or events; empty when it is fine
    QString check(int type, bool hasPhasesOrEvents) const;

    // the flags --from --to --start-ago --start-unit --end-ago --end-unit
    // --length --ytd, replacing the side(s) they give. --length takes the
    // place of the side not given; given alone it replaces the end, unless
    // the start already is the length before the end
    bool applyArgs(const QJsonObject &args, QString &error);

    // describes the definition as JSON (start, end, length) and as text
    QJsonObject json(bool aligned = false) const;
    QString startText(bool aligned = false) const;
    QString endText(bool aligned = false) const;
};

// the flag names applyArgs reads
QStringList seasonRangeArgs();

// "1y2m3d", or any of the parts, in the dialog's ranges (years 0-50,
// months 0-12, days 0-31)
bool parseSeasonLength(const QString &text, SeasonLength &length, QString &error);
QString seasonLengthText(const SeasonLength &length);

// weeks, months, years
bool parseSeasonUnit(const QString &text, SeasonOffset::SeasonOffsetType &unit);
QString seasonUnitName(SeasonOffset::SeasonOffsetType unit);
QStringList seasonUnitNames();

// the type names the commands take and show: season, cycle, adhoc and
// system (the built-in ranges); phase, prep, base, build, peak, camp
QString seasonTypeName(int type);
QString phaseTypeName(int type);
bool parsePhaseType(const QString &text, int &type);
QStringList phaseTypeNames();   // the ones the Edit Phase dialog offers

// A-E, or blank; the index in SeasonEvent::priorityList()
bool parseEventPriority(const QString &text, int &priority);
QString eventPriorityName(int priority);

} // namespace Headless

#endif
