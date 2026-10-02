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

//
// Seasons, phases and events: the date ranges of the Trends sidebar, stored
// in the athlete's config/seasons.xml (Seasons, SeasonParser). The built-in
// ranges (All Dates, This Year ...) are made in code and can't be changed.
// What can be set is what the Edit Date Range, Edit Phase and Edit Event
// dialogs allow, and changes are saved as the sidebar saves them.
//

#include "HeadlessCommands.h"
#include "SeasonDefinition.h"
#include "SeasonRange.h"

#include "Athlete.h"
#include "Season.h"
#include "Seasons.h"

#include <algorithm>

namespace Headless {

static Seasons *
seasonsOf(CommandEnvironment &env)
{
    return env.session->athlete()->seasons;
}

static QString
seasonsFile(CommandEnvironment &env)
{
    return env.session->athlete()->home->config().canonicalPath() + "/seasons.xml";
}

static bool
isBuiltin(const Season &season)
{
    return season.getType() == Season::temporary;
}

static QString
day(const QDate &d)
{
    return d.isValid() ? d.toString(Qt::ISODate) : QString();
}

//
// JSON and text
//

static QJsonObject
phaseJson(const Season &season, const Phase &phase)
{
    QJsonObject o;
    o.insert("name", phase.getName());
    o.insert("id", phase.id().toString());
    o.insert("type", phaseTypeName(phase.getType()));
    o.insert("start", day(phase.getStart()));
    o.insert("end", day(phase.getEnd()));
    o.insert("seed", phase.getSeed());
    o.insert("low", phase.getLow());
    o.insert("season", season.getName());
    return o;
}

static QJsonObject
eventJson(const Season &season, const SeasonEvent &event)
{
    QJsonObject o;
    o.insert("name", event.name);
    o.insert("id", event.id);
    o.insert("date", day(event.date));
    o.insert("priority", eventPriorityName(event.priority));
    o.insert("description", event.description);
    o.insert("season", season.getName());
    o.insert("season_id", season.id().toString());
    return o;
}

static QJsonObject
seasonJson(const Season &season)
{
    bool builtin = isBuiltin(season);
    QJsonObject o;
    o.insert("name", season.getName());
    o.insert("id", season.id().toString());
    o.insert("type", seasonTypeName(season.getType()));
    o.insert("builtin", builtin);
    o.insert("start", day(season.getStart()));
    o.insert("end", day(season.getEnd()));
    o.insert("absolute", season.isAbsolute());
    // the built-in ranges are aligned to the start of the week, month or year
    SeasonDefinition def = SeasonDefinition::of(season);
    QJsonObject d = def.json(builtin);
    o.insert("definition", d);
    o.insert("seed", season.getSeed());
    o.insert("low", season.getLow());

    QJsonArray phases;
    for (const Phase &p : season.phases) phases.append(phaseJson(season, p));
    o.insert("phases", phases);
    QJsonArray events;
    for (const SeasonEvent &e : season.events) events.append(eventJson(season, e));
    o.insert("events", events);
    Season copy = season;
    QJsonArray load;
    for (int v : copy.load()) load.append(v);
    if (!load.isEmpty()) o.insert("load", load);
    return o;
}

static QString
padded(const QStringList &cells, const QList<int> &widths)
{
    QString line;
    for (int i = 0; i < cells.count(); i++) {
        if (i) line += "  ";
        line += (i < widths.count() && i < cells.count() - 1) ? cells.at(i).leftJustified(widths.at(i)) : cells.at(i);
    }
    return line.trimmed().isEmpty() ? line : line + "\n";
}

static QString
rowsText(const QList<QStringList> &rows)
{
    QList<int> widths;
    for (const QStringList &r : rows)
        for (int i = 0; i < r.count(); i++) {
            if (widths.count() <= i) widths << 0;
            widths[i] = std::max(widths[i], int(r.at(i).length()));
        }
    QString text;
    for (const QStringList &r : rows) text += padded(r, widths);
    return text;
}

static QString
seasonText(const Season &season)
{
    bool builtin = isBuiltin(season);
    SeasonDefinition def = SeasonDefinition::of(season);
    QString text;
    text += QString("%1 (%2%3)\n").arg(season.getName(), seasonTypeName(season.getType()), builtin ? ", built-in" : "");
    text += QString("  id     %1\n").arg(season.id().toString());
    text += QString("  start  %1%2\n").arg(day(season.getStart()),
                                          def.start.kind == SeasonBound::Absolute ? QString() : "  (" + def.startText(builtin) + ")");
    text += QString("  end    %1%2\n").arg(day(season.getEnd()),
                                          def.end.kind == SeasonBound::Absolute ? QString() : "  (" + def.endText(builtin) + ")");
    text += QString("  seed   %1 (starting LTS)\n").arg(season.getSeed());
    text += QString("  low    %1 (lowest SB)\n").arg(season.getLow());
    if (!season.phases.isEmpty()) {
        text += "phases:\n";
        QList<QStringList> rows;
        for (const Phase &p : season.phases)
            rows << QStringList{ "", p.getName(), phaseTypeName(p.getType()), day(p.getStart()), day(p.getEnd()),
                                 QString("seed %1").arg(p.getSeed()), QString("low %1").arg(p.getLow()), p.id().toString() };
        text += rowsText(rows);
    }
    if (!season.events.isEmpty()) {
        text += "events:\n";
        QList<QStringList> rows;
        for (const SeasonEvent &e : season.events)
            rows << QStringList{ "", day(e.date), eventPriorityName(e.priority).isEmpty() ? "-" : eventPriorityName(e.priority),
                                 e.name, e.id };
        text += rowsText(rows);
    }
    Season copy = season;
    if (!copy.load().isEmpty()) {
        QStringList values;
        for (int v : copy.load()) values << QString::number(v);
        text += QString("load:\n  %1\n").arg(values.join(" "));
    }
    return text;
}

//
// saving
//

// writes seasons.xml, or puts the seasons back as they were and fails
static CommandResult
saveSeasons(CommandEnvironment &env, const QList<Season> &before)
{
    Seasons *seasons = seasonsOf(env);
    QString error;
    if (!seasons->writeSeasons(&error)) {
        seasons->seasons = before;
        return CommandResult::failure(Status::Failed, error.isEmpty() ? QString("can't write %1").arg(seasonsFile(env)) : error);
    }
    return CommandResult::success();
}

static CommandResult
saved(CommandEnvironment &env, const QList<Season> &before, QJsonObject data, const QString &status, const QString &text)
{
    CommandResult written = saveSeasons(env, before);
    if (!written.ok()) return written;
    data.insert("status", status);
    data.insert("file", seasonsFile(env));
    CommandResult result = CommandResult::success(data);
    result.text = text;
    return result;
}

//
// checks shared by add and edit
//

static bool
seedAndLow(const QJsonObject &args, QString &error)
{
    // the ranges of the dialogs' spin boxes
    if (args.contains("seed") && (args.value("seed").toInt() < 0 || args.value("seed").toInt() > 300)) {
        error = "--seed (starting LTS) is 0 to 300";
        return false;
    }
    if (args.contains("low") && (args.value("low").toInt() < -500 || args.value("low").toInt() > 0)) {
        error = "--low (lowest SB) is -500 to 0";
        return false;
    }
    return true;
}

static bool
seasonNameTaken(const QList<Season> &seasons, const QString &name, int except)
{
    for (int i = 0; i < seasons.count(); i++)
        if (i != except && seasons.at(i).getName().compare(name, Qt::CaseInsensitive) == 0) return true;
    return false;
}

static bool
hasRangeArgs(const QJsonObject &args)
{
    for (const QString &a : seasonRangeArgs()) if (args.contains(a)) return true;
    return false;
}

static int
seasonType(const QString &name)
{
    if (name == "cycle") return Season::cycle;
    if (name == "adhoc") return Season::adhoc;
    return Season::season;
}

static bool
rangeInOrder(const Season &season, QString &error)
{
    if (season.getStart() > season.getEnd()) {
        error = QString("the season would start (%1) after it ends (%2)").arg(day(season.getStart()), day(season.getEnd()));
        return false;
    }
    return true;
}

static bool
userSeason(const QList<Season> &seasons, const QJsonObject &args, const QString &key, int &index,
           QString &error, Status &status)
{
    SeasonMatch match;
    if (!findOneSeason(seasons, args.value(key).toString(), false, match, error, status)) return false;
    if (isBuiltin(seasons.at(match.season))) {
        error = QString("'%1' is a built-in date range: only seasons you created can be changed").arg(seasons.at(match.season).getName());
        status = Status::Usage;
        return false;
    }
    index = match.season;
    return true;
}

//
// seasons
//

static CommandResult
listSeasons(CommandEnvironment &env, const CommandRequest &)
{
    const QList<Season> &seasons = seasonsOf(env)->seasons;
    QJsonArray list;
    QList<QStringList> rows;
    for (const Season &s : seasons) {
        QJsonObject o;
        o.insert("kind", "season");
        o.insert("name", s.getName());
        o.insert("id", s.id().toString());
        o.insert("type", seasonTypeName(s.getType()));
        o.insert("builtin", isBuiltin(s));
        o.insert("start", day(s.getStart()));
        o.insert("end", day(s.getEnd()));
        o.insert("season", QJsonValue::Null);
        list.append(o);
        rows << QStringList{ s.getName(), seasonTypeName(s.getType()), day(s.getStart()), day(s.getEnd()),
                             isBuiltin(s) ? "built-in" : s.id().toString() };
        for (const Phase &p : s.phases) {
            QJsonObject po;
            po.insert("kind", "phase");
            po.insert("name", p.getName());
            po.insert("id", p.id().toString());
            po.insert("type", phaseTypeName(p.getType()));
            po.insert("builtin", false);
            po.insert("start", day(p.getStart()));
            po.insert("end", day(p.getEnd()));
            po.insert("season", s.getName());
            list.append(po);
            rows << QStringList{ "  " + p.getName(), phaseTypeName(p.getType()), day(p.getStart()), day(p.getEnd()),
                                 p.id().toString() };
        }
    }
    QJsonObject data;
    data.insert("seasons", list);
    CommandResult result = CommandResult::success(data);
    result.text = rowsText(rows);
    return result;
}

static CommandResult
showSeason(CommandEnvironment &env, const CommandRequest &request)
{
    const QList<Season> &seasons = seasonsOf(env)->seasons;
    SeasonMatch match;
    QString error;
    Status status;
    if (!findOneSeason(seasons, request.args.value("season").toString(), false, match, error, status))
        return CommandResult::failure(status, error);
    const Season &season = seasons.at(match.season);
    CommandResult result = CommandResult::success(seasonJson(season));
    result.text = seasonText(season);
    return result;
}

static CommandResult
addSeason(CommandEnvironment &env, const CommandRequest &request)
{
    const QJsonObject &args = request.args;
    Seasons *seasons = seasonsOf(env);
    QString name = args.value("name").toString().trimmed();
    if (name.isEmpty()) return CommandResult::failure(Status::Usage, "the name is empty");
    if (seasonNameTaken(seasons->seasons, name, -1))
        return CommandResult::failure(Status::Usage, QString("a season called '%1' already exists").arg(name));

    QString error;
    if (!seedAndLow(args, error)) return CommandResult::failure(Status::Usage, error);
    int type = seasonType(args.value("type").toString("season"));
    SeasonDefinition def;
    if (!def.applyArgs(args, error)) return CommandResult::failure(Status::Usage, error);
    error = def.check(type, false);
    if (!error.isEmpty()) return CommandResult::failure(Status::Usage, error);

    // as the sidebar's Add season: the dialog fills a new Season
    Season add;
    add.setName(name);
    add.setType(type);
    add.setSeed(args.value("seed").toInt(0));
    add.setLow(args.value("low").toInt(-50));
    def.applyTo(add);
    if (!rangeInOrder(add, error)) return CommandResult::failure(Status::Usage, error);

    QList<Season> before = seasons->seasons;
    seasons->seasons.insert(0, add); // the sidebar adds at the top
    return saved(env, before, seasonJson(add), "added", QString("added %1 (%2 to %3)\n")
                 .arg(name, day(add.getStart()), day(add.getEnd())));
}

static CommandResult
editSeason(CommandEnvironment &env, const CommandRequest &request)
{
    const QJsonObject &args = request.args;
    bool range = hasRangeArgs(args);
    bool anything = range;
    for (const char *a : { "name", "type", "seed", "low" }) anything = anything || args.contains(a);
    if (!anything) return CommandResult::failure(Status::Usage, "give what to change: --name, --type, the dates, --seed or --low");

    Seasons *seasons = seasonsOf(env);
    QString error;
    Status status;
    int index;
    if (!userSeason(seasons->seasons, args, "season", index, error, status)) return CommandResult::failure(status, error);
    if (!seedAndLow(args, error)) return CommandResult::failure(Status::Usage, error);

    Season season = seasons->seasons.at(index);
    if (args.contains("name")) {
        QString name = args.value("name").toString().trimmed();
        if (name.isEmpty()) return CommandResult::failure(Status::Usage, "the name is empty");
        if (seasonNameTaken(seasons->seasons, name, index))
            return CommandResult::failure(Status::Usage, QString("a season called '%1' already exists").arg(name));
        season.setName(name);
    }
    int type = args.contains("type") ? seasonType(args.value("type").toString()) : season.getType();
    if (range || type != season.getType()) {
        SeasonDefinition def = SeasonDefinition::of(season);
        if (!def.applyArgs(args, error)) return CommandResult::failure(Status::Usage, error);
        error = def.check(type, !season.phases.isEmpty() || !season.events.isEmpty());
        if (!error.isEmpty()) return CommandResult::failure(Status::Usage, error);
        if (range) def.applyTo(season);
    }
    season.setType(type);
    if (args.contains("seed")) season.setSeed(args.value("seed").toInt());
    if (args.contains("low")) season.setLow(args.value("low").toInt());
    if (!rangeInOrder(season, error)) return CommandResult::failure(Status::Usage, error);

    QList<Season> before = seasons->seasons;
    seasons->seasons[index] = season;
    return saved(env, before, seasonJson(season), "updated", QString("updated %1 (%2 to %3)\n")
                 .arg(season.getName(), day(season.getStart()), day(season.getEnd())));
}

static CommandResult
removeSeason(CommandEnvironment &env, const CommandRequest &request)
{
    Seasons *seasons = seasonsOf(env);
    QString error;
    Status status;
    int index;
    if (!userSeason(seasons->seasons, request.args, "season", index, error, status)) return CommandResult::failure(status, error);

    const Season season = seasons->seasons.at(index);
    QList<Season> before = seasons->seasons;
    seasons->seasons.removeAt(index);
    QJsonObject data{ { "name", season.getName() }, { "id", season.id().toString() } };
    return saved(env, before, data, "removed", QString("removed %1\n").arg(season.getName()));
}

//
// phases
//

static bool
findPhase(const Season &season, const QString &key, int &index, QString &error, Status &status)
{
    QList<int> found;
    QUuid id(key.trimmed());
    for (int i = 0; i < season.phases.count(); i++) {
        const Phase &p = season.phases.at(i);
        if ((!id.isNull() && p.id() == id) || p.getName().compare(key.trimmed(), Qt::CaseInsensitive) == 0) found << i;
    }
    if (found.count() == 1) {
        index = found.first();
        return true;
    }
    if (found.isEmpty()) {
        error = QString("'%1' has no phase called '%2'").arg(season.getName(), key);
        status = Status::NotFound;
    } else {
        QStringList names;
        for (int i : found) names << season.phases.at(i).getName() + " " + season.phases.at(i).id().toString();
        error = QString("'%1' matches more than one phase, give the id: %2").arg(key, names.join(", "));
        status = Status::Usage;
    }
    return false;
}

static bool
phaseNameTaken(const Season &season, const QString &name, int except)
{
    for (int i = 0; i < season.phases.count(); i++)
        if (i != except && season.phases.at(i).getName().compare(name, Qt::CaseInsensitive) == 0) return true;
    return false;
}

// as the Edit Phase dialog's date ranges allow: inside the season, at least a day long
static bool
phaseDates(const Season &season, const Phase &phase, QString &error)
{
    if (phase.getStart() < season.getStart() || phase.getEnd() > season.getEnd()) {
        error = QString("a phase lies within its season, %1 to %2").arg(day(season.getStart()), day(season.getEnd()));
        return false;
    }
    if (phase.getStart() >= phase.getEnd()) {
        error = QString("a phase ends at least a day after it starts (%1 to %2)").arg(day(phase.getStart()), day(phase.getEnd()));
        return false;
    }
    return true;
}

// phases and events go on seasons you created with fixed dates, as the sidebar
static bool
canHavePhasesOrEvents(const Season &season, const QString &what, QString &error)
{
    if (season.canHavePhasesOrEvents()) return true;
    if (isBuiltin(season)) error = QString("'%1' is a built-in date range: %2 can only be added to seasons you created").arg(season.getName(), what);
    else error = QString("'%1' moves with today: %2 can only be added to a season with fixed dates").arg(season.getName(), what);
    return false;
}

static CommandResult
addPhase(CommandEnvironment &env, const CommandRequest &request)
{
    const QJsonObject &args = request.args;
    Seasons *seasons = seasonsOf(env);
    SeasonMatch match;
    QString error;
    Status status;
    if (!findOneSeason(seasons->seasons, args.value("season").toString(), false, match, error, status))
        return CommandResult::failure(status, error);
    // a copy, changed and put back: a reference into the list would change
    // the implicitly shared copy kept for undoing a failed write too
    Season season = seasons->seasons.at(match.season);
    if (!canHavePhasesOrEvents(season, "phases", error)) return CommandResult::failure(Status::Usage, error);

    QString name = args.value("name").toString().trimmed();
    if (name.isEmpty()) return CommandResult::failure(Status::Usage, "the name is empty");
    if (phaseNameTaken(season, name, -1))
        return CommandResult::failure(Status::Usage, QString("'%1' already has a phase called '%2'").arg(season.getName(), name));
    if (!seedAndLow(args, error)) return CommandResult::failure(Status::Usage, error);
    int type = Phase::phase;
    parsePhaseType(args.value("type").toString("phase"), type);

    // the sidebar starts a new phase on the season's dates
    QDate from = args.contains("from") ? QDate::fromString(args.value("from").toString(), Qt::ISODate) : season.getStart();
    QDate to = args.contains("to") ? QDate::fromString(args.value("to").toString(), Qt::ISODate) : season.getEnd();
    Phase phase(name, from, to);
    phase.setType(type);
    phase.setSeed(args.value("seed").toInt(0));
    phase.setLow(args.value("low").toInt(-50));
    if (!phaseDates(season, phase, error)) return CommandResult::failure(Status::Usage, error);

    QList<Season> before = seasons->seasons;
    season.phases.append(phase);
    seasons->seasons[match.season] = season;
    return saved(env, before, phaseJson(season, phase), "added",
                 QString("added phase %1 to %2 (%3 to %4)\n").arg(name, season.getName(), day(from), day(to)));
}

static CommandResult
editPhase(CommandEnvironment &env, const CommandRequest &request)
{
    const QJsonObject &args = request.args;
    bool anything = false;
    for (const char *a : { "name", "type", "from", "to", "seed", "low" }) anything = anything || args.contains(a);
    if (!anything) return CommandResult::failure(Status::Usage, "give what to change: --name, --type, --from, --to, --seed or --low");

    Seasons *seasons = seasonsOf(env);
    SeasonMatch match;
    QString error;
    Status status;
    if (!findOneSeason(seasons->seasons, args.value("season").toString(), false, match, error, status))
        return CommandResult::failure(status, error);
    // a copy, changed and put back: a reference into the list would change
    // the implicitly shared copy kept for undoing a failed write too
    Season season = seasons->seasons.at(match.season);
    int index;
    if (!findPhase(season, args.value("phase").toString(), index, error, status)) return CommandResult::failure(status, error);
    if (!seedAndLow(args, error)) return CommandResult::failure(Status::Usage, error);

    Phase phase = season.phases.at(index);
    if (args.contains("name")) {
        QString name = args.value("name").toString().trimmed();
        if (name.isEmpty()) return CommandResult::failure(Status::Usage, "the name is empty");
        if (phaseNameTaken(season, name, index))
            return CommandResult::failure(Status::Usage, QString("'%1' already has a phase called '%2'").arg(season.getName(), name));
        phase.setName(name);
    }
    if (args.contains("type")) {
        int type = Phase::phase;
        parsePhaseType(args.value("type").toString(), type);
        phase.setType(type);
    }
    if (args.contains("from")) phase.setAbsoluteStart(QDate::fromString(args.value("from").toString(), Qt::ISODate));
    if (args.contains("to")) phase.setAbsoluteEnd(QDate::fromString(args.value("to").toString(), Qt::ISODate));
    if (args.contains("seed")) phase.setSeed(args.value("seed").toInt());
    if (args.contains("low")) phase.setLow(args.value("low").toInt());
    if ((args.contains("from") || args.contains("to")) && !phaseDates(season, phase, error))
        return CommandResult::failure(Status::Usage, error);

    QList<Season> before = seasons->seasons;
    season.phases[index] = phase;
    seasons->seasons[match.season] = season;
    return saved(env, before, phaseJson(season, phase), "updated",
                 QString("updated phase %1 of %2\n").arg(phase.getName(), season.getName()));
}

static CommandResult
removePhase(CommandEnvironment &env, const CommandRequest &request)
{
    Seasons *seasons = seasonsOf(env);
    SeasonMatch match;
    QString error;
    Status status;
    if (!findOneSeason(seasons->seasons, request.args.value("season").toString(), false, match, error, status))
        return CommandResult::failure(status, error);
    // a copy, changed and put back: a reference into the list would change
    // the implicitly shared copy kept for undoing a failed write too
    Season season = seasons->seasons.at(match.season);
    int index;
    if (!findPhase(season, request.args.value("phase").toString(), index, error, status))
        return CommandResult::failure(status, error);

    const Phase phase = season.phases.at(index);
    QList<Season> before = seasons->seasons;
    season.phases.removeAt(index);
    seasons->seasons[match.season] = season;
    QJsonObject data{ { "name", phase.getName() }, { "id", phase.id().toString() }, { "season", season.getName() } };
    return saved(env, before, data, "removed", QString("removed phase %1 of %2\n").arg(phase.getName(), season.getName()));
}

//
// events
//

struct EventMatch {
    int season = -1;
    int event = -1;
};

static bool
sameEventId(const QString &id, const QString &key)
{
    if (id == key) return true;
    QUuid a(id), b(key);
    return !a.isNull() && a == b;
}

static bool
findEvent(const QList<Season> &seasons, const QJsonObject &args, EventMatch &match, QString &error, Status &status)
{
    int only = -1;
    if (args.contains("season")) {
        SeasonMatch sm;
        if (!findOneSeason(seasons, args.value("season").toString(), false, sm, error, status)) return false;
        only = sm.season;
    }
    QString key = args.value("event").toString().trimmed();
    QList<EventMatch> found;
    for (int i = 0; i < seasons.count(); i++) {
        if (only >= 0 && i != only) continue;
        for (int j = 0; j < seasons.at(i).events.count(); j++) {
            const SeasonEvent &e = seasons.at(i).events.at(j);
            if (sameEventId(e.id, key) || e.name.compare(key, Qt::CaseInsensitive) == 0) found << EventMatch{ i, j };
        }
    }
    if (found.count() == 1) {
        match = found.first();
        return true;
    }
    if (found.isEmpty()) {
        error = QString("no event called '%1', see 'event list'").arg(key);
        status = Status::NotFound;
    } else {
        QStringList names;
        for (const EventMatch &m : found) {
            const SeasonEvent &e = seasons.at(m.season).events.at(m.event);
            names << QString("%1 on %2 in %3 %4").arg(e.name, day(e.date), seasons.at(m.season).getName(), e.id);
        }
        error = QString("'%1' matches more than one event, give the id (or --season): %2").arg(key, names.join(", "));
        status = Status::Usage;
    }
    return false;
}

// as the Edit Event dialog's date range: within the season
static bool
eventDate(const Season &season, const QDate &date, QString &error)
{
    if (date < season.getStart() || date > season.getEnd()) {
        error = QString("an event lies within its season, %1 to %2").arg(day(season.getStart()), day(season.getEnd()));
        return false;
    }
    return true;
}

static QString
eventLine(const Season &season, const SeasonEvent &e)
{
    QString p = eventPriorityName(e.priority);
    return QString("%1  %2  %3  (%4)  %5\n").arg(day(e.date), p.isEmpty() ? "-" : p, e.name, season.getName(), e.id);
}

static CommandResult
listEvents(CommandEnvironment &env, const CommandRequest &request)
{
    const QJsonObject &args = request.args;
    const QList<Season> &seasons = seasonsOf(env)->seasons;
    int only = -1;
    QString error;
    Status status;
    if (args.contains("season")) {
        SeasonMatch sm;
        if (!findOneSeason(seasons, args.value("season").toString(), false, sm, error, status))
            return CommandResult::failure(status, error);
        only = sm.season;
    }
    QDate from = QDate::fromString(args.value("from").toString(), Qt::ISODate);
    QDate to = QDate::fromString(args.value("to").toString(), Qt::ISODate);

    QList<EventMatch> list;
    for (int i = 0; i < seasons.count(); i++) {
        if (only >= 0 && i != only) continue;
        for (int j = 0; j < seasons.at(i).events.count(); j++) {
            const QDate d = seasons.at(i).events.at(j).date;
            if (from.isValid() && d < from) continue;
            if (to.isValid() && d > to) continue;
            list << EventMatch{ i, j };
        }
    }
    std::stable_sort(list.begin(), list.end(), [&](const EventMatch &a, const EventMatch &b) {
        return seasons.at(a.season).events.at(a.event).date < seasons.at(b.season).events.at(b.event).date;
    });

    QJsonArray events;
    QString text;
    for (const EventMatch &m : list) {
        const Season &s = seasons.at(m.season);
        events.append(eventJson(s, s.events.at(m.event)));
        text += eventLine(s, s.events.at(m.event));
    }
    QJsonObject data;
    data.insert("events", events);
    CommandResult result = CommandResult::success(data);
    result.text = text.isEmpty() ? "(none)\n" : text;
    return result;
}

static CommandResult
addEvent(CommandEnvironment &env, const CommandRequest &request)
{
    const QJsonObject &args = request.args;
    Seasons *seasons = seasonsOf(env);
    SeasonMatch match;
    QString error;
    Status status;
    if (!findOneSeason(seasons->seasons, args.value("season").toString(), false, match, error, status))
        return CommandResult::failure(status, error);
    // a copy, changed and put back: a reference into the list would change
    // the implicitly shared copy kept for undoing a failed write too
    Season season = seasons->seasons.at(match.season);
    if (!canHavePhasesOrEvents(season, "events", error)) return CommandResult::failure(Status::Usage, error);

    QString name = args.value("name").toString().trimmed();
    if (name.isEmpty()) return CommandResult::failure(Status::Usage, "the name is empty");
    int priority = 0;
    if (!parseEventPriority(args.value("priority").toString(), priority))
        return CommandResult::failure(Status::Usage, "--priority is A, B, C, D, E or none");
    // the sidebar puts a new event on the season's last day
    QDate date = args.contains("date") ? QDate::fromString(args.value("date").toString(), Qt::ISODate) : season.getEnd();
    if (!eventDate(season, date, error)) return CommandResult::failure(Status::Usage, error);

    // the constructor gives it a new id, as in the GUI
    SeasonEvent event(name, date, priority, args.value("description").toString());
    QList<Season> before = seasons->seasons;
    season.events.append(event);
    seasons->seasons[match.season] = season;
    return saved(env, before, eventJson(season, event), "added",
                 QString("added event %1 on %2 to %3\n").arg(name, day(date), season.getName()));
}

static CommandResult
editEvent(CommandEnvironment &env, const CommandRequest &request)
{
    const QJsonObject &args = request.args;
    bool anything = false;
    for (const char *a : { "name", "date", "priority", "description" }) anything = anything || args.contains(a);
    if (!anything) return CommandResult::failure(Status::Usage, "give what to change: --name, --date, --priority or --description");

    Seasons *seasons = seasonsOf(env);
    EventMatch match;
    QString error;
    Status status;
    if (!findEvent(seasons->seasons, args, match, error, status)) return CommandResult::failure(status, error);
    // a copy, changed and put back: a reference into the list would change
    // the implicitly shared copy kept for undoing a failed write too
    Season season = seasons->seasons.at(match.season);
    SeasonEvent event = season.events.at(match.event);

    if (args.contains("name")) {
        QString name = args.value("name").toString().trimmed();
        if (name.isEmpty()) return CommandResult::failure(Status::Usage, "the name is empty");
        event.name = name;
    }
    if (args.contains("date")) {
        event.date = QDate::fromString(args.value("date").toString(), Qt::ISODate);
        if (!eventDate(season, event.date, error)) return CommandResult::failure(Status::Usage, error);
    }
    if (args.contains("priority") && !parseEventPriority(args.value("priority").toString(), event.priority))
        return CommandResult::failure(Status::Usage, "--priority is A, B, C, D, E or none");
    if (args.contains("description")) event.description = args.value("description").toString();

    QList<Season> before = seasons->seasons;
    season.events[match.event] = event;
    seasons->seasons[match.season] = season;
    return saved(env, before, eventJson(season, event), "updated",
                 QString("updated event %1 on %2 in %3\n").arg(event.name, day(event.date), season.getName()));
}

static CommandResult
removeEvent(CommandEnvironment &env, const CommandRequest &request)
{
    Seasons *seasons = seasonsOf(env);
    EventMatch match;
    QString error;
    Status status;
    if (!findEvent(seasons->seasons, request.args, match, error, status)) return CommandResult::failure(status, error);
    // a copy, changed and put back: a reference into the list would change
    // the implicitly shared copy kept for undoing a failed write too
    Season season = seasons->seasons.at(match.season);
    const SeasonEvent event = season.events.at(match.event);

    QList<Season> before = seasons->seasons;
    season.events.removeAt(match.event);
    seasons->seasons[match.season] = season;
    QJsonObject data{ { "name", event.name }, { "id", event.id }, { "season", season.getName() } };
    return saved(env, before, data, "removed", QString("removed event %1 from %2\n").arg(event.name, season.getName()));
}

//
// the table
//

static void
rangeParams(CommandSpec &spec)
{
    spec.params << ParamSpec("from", ParamType::Date, "a fixed start");
    spec.params << ParamSpec("to", ParamType::Date, "a fixed end");
    spec.params << ParamSpec("start-ago", ParamType::Int, "start this many --start-unit before today (0-52)");
    spec.params << ParamSpec("start-unit", ParamType::String, "weeks, months or years (default weeks)").oneOf(seasonUnitNames());
    spec.params << ParamSpec("end-ago", ParamType::Int, "end this many --end-unit before today (0-52)");
    spec.params << ParamSpec("end-unit", ParamType::String, "weeks, months or years (default weeks)").oneOf(seasonUnitNames());
    spec.params << ParamSpec("length", ParamType::String,
                             "a length such as 1y2m3d, 6m or 10d: after the start, or before the end when only an end is given");
    spec.params << ParamSpec("ytd", ParamType::Bool, "end on today's day and month in the start's year (year to date)");
    spec.params << ParamSpec("seed", ParamType::Int, "starting LTS (CTL) for the PMC on the first day, 0-300");
    spec.params << ParamSpec("low", ParamType::Int, "lowest SB, -500 to 0");
}

static const char *rangeHelp =
    "The start is --from DATE, --start-ago N (--start-unit weeks|months|years),\n"
    "or --length before the end. The end is --to DATE, --end-ago N\n"
    "(--end-unit), --length after the start, or --ytd. These are the choices of\n"
    "the GUI's Edit Date Range dialog. A cycle or an adhoc range has fixed\n"
    "dates; a season with phases or events can't move with today.";

void
registerSeasonCommands(CommandRegistry &registry)
{
    Command list;
    list.spec.name = "season.list";
    list.spec.summary = "list seasons and date ranges, with their phases, as of today";
    list.spec.description =
        "The seasons you created come first, then the built-in ranges (All Dates,\n"
        "This Year, Last 6 weeks ...). Each phase follows its season. Relative\n"
        "ranges are resolved as of today.";
    list.spec.scope = Scope::Athlete;
    list.spec.httpMethod = "GET";
    list.spec.httpPath = "/athletes/{athlete}/seasons";
    list.handler = listSeasons;
    registry.add(list);

    Command show;
    show.spec.name = "season.show";
    show.spec.summary = "show a season: its dates and how they are defined, seed and low, phases, events";
    show.spec.scope = Scope::Athlete;
    show.spec.params << ParamSpec("season", ParamType::String, "season name (case-insensitive) or id").req().pos();
    show.spec.httpMethod = "GET";
    show.spec.httpPath = "/athletes/{athlete}/seasons/{season}";
    show.handler = showSeason;
    registry.add(show);

    Command add;
    add.spec.name = "season.add";
    add.spec.summary = "add a season, cycle or adhoc date range";
    add.spec.description = rangeHelp;
    add.spec.scope = Scope::Athlete;
    add.spec.modifies = true;
    add.spec.params << ParamSpec("name", ParamType::String, "season name").req().pos();
    add.spec.params << ParamSpec("type", ParamType::String, "kind of range").def("season").oneOf({ "season", "cycle", "adhoc" });
    rangeParams(add.spec);
    add.spec.httpMethod = "POST";
    add.spec.httpPath = "/athletes/{athlete}/seasons";
    add.handler = addSeason;
    registry.add(add);

    Command edit;
    edit.spec.name = "season.edit";
    edit.spec.summary = "change a season you created: name, type, dates, seed or low";
    edit.spec.description = QString(rangeHelp) + "\n"
        "A start or an end that is given replaces that side and keeps the other.\n"
        "--length alone changes the length, or replaces the end with one.";
    edit.spec.scope = Scope::Athlete;
    edit.spec.modifies = true;
    edit.spec.params << ParamSpec("season", ParamType::String, "season name or id").req().pos();
    edit.spec.params << ParamSpec("name", ParamType::String, "new name");
    edit.spec.params << ParamSpec("type", ParamType::String, "kind of range").oneOf({ "season", "cycle", "adhoc" });
    rangeParams(edit.spec);
    edit.spec.httpMethod = "PUT";
    edit.spec.httpPath = "/athletes/{athlete}/seasons/{season}";
    edit.handler = editSeason;
    registry.add(edit);

    Command remove;
    remove.spec.name = "season.remove";
    remove.spec.summary = "remove a season you created, with its phases and events";
    remove.spec.scope = Scope::Athlete;
    remove.spec.modifies = true;
    remove.spec.params << ParamSpec("season", ParamType::String, "season name or id").req().pos();
    remove.spec.httpMethod = "DELETE";
    remove.spec.httpPath = "/athletes/{athlete}/seasons/{season}";
    remove.handler = removeSeason;
    registry.add(remove);

    Command padd;
    padd.spec.name = "season.phase.add";
    padd.spec.summary = "add a phase to a season (Base, Build ...)";
    padd.spec.description =
        "Phases go on seasons you created with fixed dates. A phase lies within\n"
        "its season and lasts at least a day; --from and --to default to the\n"
        "season's dates, as in the GUI.";
    padd.spec.scope = Scope::Athlete;
    padd.spec.modifies = true;
    padd.spec.params << ParamSpec("season", ParamType::String, "season name or id").req().pos();
    padd.spec.params << ParamSpec("name", ParamType::String, "phase name").req().pos();
    padd.spec.params << ParamSpec("type", ParamType::String, "kind of phase").def("phase").oneOf(phaseTypeNames());
    padd.spec.params << ParamSpec("from", ParamType::Date, "first day (default: the season's)");
    padd.spec.params << ParamSpec("to", ParamType::Date, "last day (default: the season's)");
    padd.spec.params << ParamSpec("seed", ParamType::Int, "starting LTS, 0-300");
    padd.spec.params << ParamSpec("low", ParamType::Int, "lowest SB, -500 to 0");
    padd.spec.httpMethod = "POST";
    padd.spec.httpPath = "/athletes/{athlete}/seasons/{season}/phases";
    padd.handler = addPhase;
    registry.add(padd);

    Command pedit;
    pedit.spec.name = "season.phase.edit";
    pedit.spec.summary = "change a phase of a season";
    pedit.spec.scope = Scope::Athlete;
    pedit.spec.modifies = true;
    pedit.spec.params << ParamSpec("season", ParamType::String, "season name or id").req().pos();
    pedit.spec.params << ParamSpec("phase", ParamType::String, "phase name or id").req().pos();
    pedit.spec.params << ParamSpec("name", ParamType::String, "new name");
    pedit.spec.params << ParamSpec("type", ParamType::String, "kind of phase").oneOf(phaseTypeNames());
    pedit.spec.params << ParamSpec("from", ParamType::Date, "first day");
    pedit.spec.params << ParamSpec("to", ParamType::Date, "last day");
    pedit.spec.params << ParamSpec("seed", ParamType::Int, "starting LTS, 0-300");
    pedit.spec.params << ParamSpec("low", ParamType::Int, "lowest SB, -500 to 0");
    pedit.spec.httpMethod = "PUT";
    pedit.spec.httpPath = "/athletes/{athlete}/seasons/{season}/phases/{phase}";
    pedit.handler = editPhase;
    registry.add(pedit);

    Command premove;
    premove.spec.name = "season.phase.remove";
    premove.spec.summary = "remove a phase from a season";
    premove.spec.scope = Scope::Athlete;
    premove.spec.modifies = true;
    premove.spec.params << ParamSpec("season", ParamType::String, "season name or id").req().pos();
    premove.spec.params << ParamSpec("phase", ParamType::String, "phase name or id").req().pos();
    premove.spec.httpMethod = "DELETE";
    premove.spec.httpPath = "/athletes/{athlete}/seasons/{season}/phases/{phase}";
    premove.handler = removePhase;
    registry.add(premove);

    Command elist;
    elist.spec.name = "event.list";
    elist.spec.summary = "list the events of the seasons (races and other key dates), by date";
    elist.spec.scope = Scope::Athlete;
    elist.spec.params << ParamSpec("season", ParamType::String, "only the events of this season (name or id)");
    elist.spec.params << ParamSpec("from", ParamType::Date, "only events on or after this day");
    elist.spec.params << ParamSpec("to", ParamType::Date, "only events on or before this day");
    elist.spec.httpMethod = "GET";
    elist.spec.httpPath = "/athletes/{athlete}/events";
    elist.handler = listEvents;
    registry.add(elist);

    Command eadd;
    eadd.spec.name = "event.add";
    eadd.spec.summary = "add an event to a season";
    eadd.spec.description =
        "Events go on seasons you created with fixed dates, on a day within the\n"
        "season. --date defaults to the season's last day, as in the GUI.";
    eadd.spec.scope = Scope::Athlete;
    eadd.spec.modifies = true;
    eadd.spec.params << ParamSpec("season", ParamType::String, "season name or id").req().pos();
    eadd.spec.params << ParamSpec("name", ParamType::String, "event name").req().pos();
    eadd.spec.params << ParamSpec("date", ParamType::Date, "the day (default: the season's last day)");
    eadd.spec.params << ParamSpec("priority", ParamType::String, "A to E, or none").def("none");
    eadd.spec.params << ParamSpec("description", ParamType::String, "description");
    eadd.spec.httpMethod = "POST";
    eadd.spec.httpPath = "/athletes/{athlete}/seasons/{season}/events";
    eadd.handler = addEvent;
    registry.add(eadd);

    Command eedit;
    eedit.spec.name = "event.edit";
    eedit.spec.summary = "change an event";
    eedit.spec.scope = Scope::Athlete;
    eedit.spec.modifies = true;
    eedit.spec.params << ParamSpec("event", ParamType::String, "event name (case-insensitive) or id").req().pos();
    eedit.spec.params << ParamSpec("season", ParamType::String, "look for the event in this season only");
    eedit.spec.params << ParamSpec("name", ParamType::String, "new name");
    eedit.spec.params << ParamSpec("date", ParamType::Date, "new day, within the season");
    eedit.spec.params << ParamSpec("priority", ParamType::String, "A to E, or none");
    eedit.spec.params << ParamSpec("description", ParamType::String, "description");
    eedit.spec.httpMethod = "PUT";
    eedit.spec.httpPath = "/athletes/{athlete}/events/{event}";
    eedit.handler = editEvent;
    registry.add(eedit);

    Command eremove;
    eremove.spec.name = "event.remove";
    eremove.spec.summary = "remove an event";
    eremove.spec.scope = Scope::Athlete;
    eremove.spec.modifies = true;
    eremove.spec.params << ParamSpec("event", ParamType::String, "event name or id").req().pos();
    eremove.spec.params << ParamSpec("season", ParamType::String, "look for the event in this season only");
    eremove.spec.httpMethod = "DELETE";
    eremove.spec.httpPath = "/athletes/{athlete}/events/{event}";
    eremove.handler = removeEvent;
    registry.add(eremove);
}

} // namespace Headless
