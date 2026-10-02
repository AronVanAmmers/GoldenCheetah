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

#include "SeasonDefinition.h"

#include <QRegularExpression>

namespace Headless {

//
// names
//

QStringList
seasonUnitNames()
{
    return { "weeks", "months", "years" };
}

bool
parseSeasonUnit(const QString &text, SeasonOffset::SeasonOffsetType &unit)
{
    QString t = text.trimmed().toLower();
    if (t == "weeks" || t == "week") unit = SeasonOffset::week;
    else if (t == "months" || t == "month") unit = SeasonOffset::month;
    else if (t == "years" || t == "year") unit = SeasonOffset::year;
    else return false;
    return true;
}

QString
seasonUnitName(SeasonOffset::SeasonOffsetType unit)
{
    switch (unit) {
    case SeasonOffset::year: return "years";
    case SeasonOffset::month: return "months";
    default: return "weeks";
    }
}

QString
seasonTypeName(int type)
{
    if (type >= 0 && type < Season::types.count()) return Season::types.at(type).toLower();
    return QString::number(type);
}

static const QList<QPair<QString, int>> &
phaseTypes()
{
    static const QList<QPair<QString, int>> types = {
        { "phase", Phase::phase }, { "prep", Phase::prep }, { "base", Phase::base },
        { "build", Phase::build }, { "peak", Phase::peak }, { "camp", Phase::camp },
    };
    return types;
}

QString
phaseTypeName(int type)
{
    for (const auto &t : phaseTypes()) if (t.second == type) return t.first;
    return QString::number(type);
}

QStringList
phaseTypeNames()
{
    // as EditPhaseDialog offers them: no Peak
    return { "phase", "prep", "base", "build", "camp" };
}

bool
parsePhaseType(const QString &text, int &type)
{
    QString t = text.trimmed().toLower();
    if (!phaseTypeNames().contains(t)) return false;
    for (const auto &p : phaseTypes()) if (p.first == t) type = p.second;
    return true;
}

bool
parseEventPriority(const QString &text, int &priority)
{
    QString t = text.trimmed();
    if (t.isEmpty() || t.compare("none", Qt::CaseInsensitive) == 0) {
        priority = 0;
        return true;
    }
    QStringList list = SeasonEvent::priorityList();
    for (int i = 1; i < list.count(); i++) {
        if (list.at(i).trimmed().compare(t, Qt::CaseInsensitive) == 0) {
            priority = i;
            return true;
        }
    }
    return false;
}

QString
eventPriorityName(int priority)
{
    QStringList list = SeasonEvent::priorityList();
    if (priority < 0 || priority >= list.count()) return QString();
    return list.at(priority).trimmed();
}

//
// lengths
//

bool
parseSeasonLength(const QString &text, SeasonLength &length, QString &error)
{
    static const QRegularExpression re("^(?:(\\d+)y)?(?:(\\d+)m)?(?:(\\d+)d)?$");
    QString t = text.trimmed().toLower().remove(' ');
    QRegularExpressionMatch m = re.match(t);
    if (t.isEmpty() || !m.hasMatch()) {
        error = QString("bad length '%1', expected years, months and days such as 1y, 6m, 10d or 1y2m3d").arg(text);
        return false;
    }
    int years = m.captured(1).toInt();
    int months = m.captured(2).toInt();
    int days = m.captured(3).toInt();
    // the ranges of the dialog's spin boxes
    if (years > 50 || months > 12 || days > 31) {
        error = QString("length '%1' is out of range: at most 50 years, 12 months and 31 days").arg(text);
        return false;
    }
    length = SeasonLength(years, months, days);
    return true;
}

QString
seasonLengthText(const SeasonLength &length)
{
    QString text;
    if (length.getYears() > 0) text += QString("%1y").arg(length.getYears());
    if (length.getMonths() > 0) text += QString("%1m").arg(length.getMonths());
    if (length.getDays() > 0 || text.isEmpty()) text += QString("%1d").arg(std::max(0, length.getDays()));
    return text;
}

//
// the definition
//

SeasonDefinition
SeasonDefinition::of(const Season &season)
{
    SeasonDefinition d;
    d.length = season.getLength();

    std::pair<SeasonOffset::SeasonOffsetType, int> item;
    if (season.getAbsoluteStart().isValid()) {
        d.start.kind = SeasonBound::Absolute;
        d.start.date = season.getAbsoluteStart();
    } else if (season.getOffsetStart().isValid()
               && (item = season.getOffsetStart().getSignificantItem()).first != SeasonOffset::invalid) {
        d.start.kind = SeasonBound::Relative;
        d.start.unit = item.first;
        d.start.ago = -item.second;
    } else if (season.getLength().isValid()
               && (season.getAbsoluteEnd().isValid() || season.getOffsetEnd().isValid())) {
        d.start.kind = SeasonBound::Duration;
    }

    if (season.getAbsoluteEnd().isValid()) {
        d.end.kind = SeasonBound::Absolute;
        d.end.date = season.getAbsoluteEnd();
    } else if (season.getOffsetEnd().isValid()
               && (item = season.getOffsetEnd().getSignificantItem()).first != SeasonOffset::invalid) {
        d.end.kind = SeasonBound::Relative;
        d.end.unit = item.first;
        d.end.ago = -item.second;
    } else if (season.getLength().isValid()
               && (season.getAbsoluteStart().isValid() || season.getOffsetStart().isValid())) {
        d.end.kind = SeasonBound::Duration;
    } else if (season.isYtd()) {
        d.end.kind = SeasonBound::Ytd;
    }
    return d;
}

static SeasonOffset
offsetAgo(const SeasonBound &b)
{
    // the dialog's "N weeks/months/years ago": not aligned to the start of
    // the week, month or year (EditSeasonDialog::getSeasonOffset)
    return SeasonOffset(std::make_pair(b.unit, -b.ago), false);
}

void
SeasonDefinition::applyTo(Season &season) const
{
    season.resetTimeRange();
    switch (start.kind) {
    case SeasonBound::Absolute: season.setAbsoluteStart(start.date); break;
    case SeasonBound::Relative: season.setOffsetStart(offsetAgo(start)); break;
    case SeasonBound::Duration: season.setLength(length); break;
    default: break;
    }
    switch (end.kind) {
    case SeasonBound::Absolute: season.setAbsoluteEnd(end.date); break;
    case SeasonBound::Relative: season.setOffsetEnd(offsetAgo(end)); break;
    case SeasonBound::Duration: season.setLength(length); break;
    case SeasonBound::Ytd: season.setYtd(); break;
    default: break;
    }
}

QString
SeasonDefinition::check(int type, bool hasPhasesOrEvents) const
{
    if (start.kind == SeasonBound::None) return "give a start: --from, --start-ago, or --length with an end";
    if (end.kind == SeasonBound::None) return "give an end: --to, --end-ago, --ytd, or --length with a start";
    if (start.kind == SeasonBound::Duration && end.kind == SeasonBound::Ytd)
        return "--length before the end can't be combined with --ytd";
    if (type != Season::season && (start.kind != SeasonBound::Absolute || end.kind != SeasonBound::Absolute))
        return "a cycle or an adhoc range has fixed dates: give --from and --to";
    if (hasPhasesOrEvents && (start.kind == SeasonBound::Relative || end.kind == SeasonBound::Relative
                              || end.kind == SeasonBound::Ytd))
        return "a season with phases or events can't move with today: --start-ago, --end-ago and --ytd are not allowed";
    return QString();
}

QStringList
seasonRangeArgs()
{
    return { "from", "to", "start-ago", "start-unit", "end-ago", "end-unit", "length", "ytd" };
}

static bool
readAgo(const QJsonObject &args, const QString &prefix, SeasonBound &bound, QString &error)
{
    int ago = args.value(prefix + "-ago").toInt();
    // the range of the dialog's spin box
    if (ago < 0 || ago > 52) {
        error = QString("--%1-ago is 0 to 52").arg(prefix);
        return false;
    }
    SeasonOffset::SeasonOffsetType unit = SeasonOffset::week;
    if (args.contains(prefix + "-unit") && !parseSeasonUnit(args.value(prefix + "-unit").toString(), unit)) {
        error = QString("--%1-unit is weeks, months or years").arg(prefix);
        return false;
    }
    bound = SeasonBound();
    bound.kind = SeasonBound::Relative;
    bound.ago = ago;
    bound.unit = unit;
    return true;
}

bool
SeasonDefinition::applyArgs(const QJsonObject &args, QString &error)
{
    bool ytd = args.value("ytd").toBool(false);
    int starts = int(args.contains("from")) + int(args.contains("start-ago"));
    int ends = int(args.contains("to")) + int(args.contains("end-ago")) + int(ytd);
    if (starts > 1) {
        error = "give one start: --from or --start-ago";
        return false;
    }
    if (ends > 1) {
        error = "give one end: --to, --end-ago or --ytd";
        return false;
    }
    if (args.contains("start-unit") && !args.contains("start-ago")) {
        error = "--start-unit goes with --start-ago";
        return false;
    }
    if (args.contains("end-unit") && !args.contains("end-ago")) {
        error = "--end-unit goes with --end-ago";
        return false;
    }

    SeasonDefinition d = *this;
    if (args.contains("from")) {
        d.start = SeasonBound();
        d.start.kind = SeasonBound::Absolute;
        d.start.date = QDate::fromString(args.value("from").toString(), Qt::ISODate);
    } else if (args.contains("start-ago") && !readAgo(args, "start", d.start, error)) {
        return false;
    }
    if (args.contains("to")) {
        d.end = SeasonBound();
        d.end.kind = SeasonBound::Absolute;
        d.end.date = QDate::fromString(args.value("to").toString(), Qt::ISODate);
    } else if (args.contains("end-ago")) {
        if (!readAgo(args, "end", d.end, error)) return false;
    } else if (ytd) {
        d.end = SeasonBound();
        d.end.kind = SeasonBound::Ytd;
    }

    if (args.contains("length")) {
        if (!parseSeasonLength(args.value("length").toString(), d.length, error)) return false;
        if (starts && ends) {
            error = "--length takes the place of the start or the end: give it with one of them, not both";
            return false;
        }
        SeasonBound duration;
        duration.kind = SeasonBound::Duration;
        if (ends) d.start = duration;
        else if (starts) d.end = duration;
        else if (d.start.kind != SeasonBound::Duration) d.end = duration;
    }

    *this = d;
    return true;
}

static QJsonObject
boundJson(const SeasonBound &b, const SeasonLength &length, bool aligned, bool isStart)
{
    QJsonObject o;
    switch (b.kind) {
    case SeasonBound::Absolute:
        o.insert("kind", "date");
        o.insert("date", b.date.toString(Qt::ISODate));
        break;
    case SeasonBound::Relative:
        o.insert("kind", "ago");
        o.insert("ago", b.ago);
        o.insert("unit", seasonUnitName(b.unit));
        if (aligned) o.insert("aligned", true);
        break;
    case SeasonBound::Duration:
        o.insert("kind", isStart ? "length before end" : "length after start");
        o.insert("length", seasonLengthText(length));
        break;
    case SeasonBound::Ytd:
        o.insert("kind", "year to date");
        break;
    default:
        // a built-in range that ends today, or starts its length before it
        if (isStart && length.isValid()) {
            o.insert("kind", "length before end");
            o.insert("length", seasonLengthText(length));
        } else {
            o.insert("kind", "today");
        }
        break;
    }
    return o;
}

static QString
boundText(const SeasonBound &b, const SeasonLength &length, bool aligned, bool isStart)
{
    switch (b.kind) {
    case SeasonBound::Absolute: return b.date.toString(Qt::ISODate);
    case SeasonBound::Relative: {
        QString unit = seasonUnitName(b.unit);
        QString text = QString("%1 %2 ago").arg(b.ago).arg(b.ago == 1 ? unit.chopped(1) : unit);
        if (aligned) text += QString(", from the start of the %1").arg(unit.chopped(1));
        return text;
    }
    case SeasonBound::Duration:
        return QString("%1 %2").arg(seasonLengthText(length), isStart ? "before the end" : "after the start");
    case SeasonBound::Ytd: return "year to date";
    default:
        if (isStart && length.isValid()) return QString("%1 before the end").arg(seasonLengthText(length));
        return "today";
    }
}

QJsonObject
SeasonDefinition::json(bool aligned) const
{
    QJsonObject o;
    o.insert("start", boundJson(start, length, aligned, true));
    o.insert("end", boundJson(end, length, aligned, false));
    return o;
}

QString
SeasonDefinition::startText(bool aligned) const
{
    return boundText(start, length, aligned, true);
}

QString
SeasonDefinition::endText(bool aligned) const
{
    return boundText(end, length, aligned, false);
}

} // namespace Headless
