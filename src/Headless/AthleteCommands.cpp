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

#include "HeadlessCommands.h"
#include "HeadlessApp.h"
#include "AthleteLock.h"
#include "ActivitySelection.h"

#include "Context.h"
#include "Athlete.h"
#include "RideCache.h"
#include "RideItem.h"
#include "Settings.h"
#include "GcUpgrade.h"
#include "Zones.h"
#include "HrZones.h"
#include "PaceZones.h"

#include <QDir>
#include <QFileInfo>

namespace Headless {

static CommandResult
listAthletes(CommandEnvironment &env, const CommandRequest &)
{
    if (!QFileInfo(env.home).isDir())
        return CommandResult::failure(Status::NotFound, QString("athletes folder '%1' does not exist").arg(env.home));

    QJsonArray list;
    for (const QString &name : HeadlessApp::athletes(env.home)) {
        QString folder = QDir(env.home).absoluteFilePath(name);
        QJsonObject o;
        o.insert("name", name);
        o.insert("folder", folder);

        int count = QDir(folder + "/activities").entryList(QDir::Files).count();
        o.insert("activity_files", count);

        // is someone else using it?
        if (!AthleteLock::heldByThisProcess(folder)) {
            AthleteLock probe(folder);
            if (!probe.tryLock(0)) o.insert("in_use_by", probe.holder());
        }
        list.append(o);
    }
    QJsonObject data;
    data.insert("home", env.home);
    data.insert("athletes", list);
    return CommandResult::success(data);
}

static CommandResult
createAthlete(CommandEnvironment &env, const CommandRequest &request)
{
    QString name = request.args.value("name").toString().trimmed();
    if (name.isEmpty() || name.contains('/') || name.contains('\\') || name.startsWith("."))
        return CommandResult::failure(Status::Usage, QString("'%1' is not a valid athlete name").arg(name));

    // the athletes folder may not exist yet
    QDir root(env.home);
    if (!root.exists() && !QDir().mkpath(env.home))
        return CommandResult::failure(Status::Failed, QString("can't create %1").arg(env.home));

    if (!HeadlessApp::isInitialised()) {
        QString error;
        if (!HeadlessApp::initialise(env.home, HeadlessApp::Options(), error))
            return CommandResult::failure(Status::Failed, error);
    }

    if (root.exists(name)) return CommandResult::failure(Status::Failed, QString("athlete '%1' already exists").arg(name));
    if (!root.mkdir(name)) return CommandResult::failure(Status::Failed, QString("can't create %1").arg(root.absoluteFilePath(name)));

    // what the new athlete wizard does
    QDir athleteDir(root.canonicalPath() + "/" + name);
    AthleteDirectoryStructure dirs(athleteDir);
    dirs.createAllSubdirs();

    appsettings->initializeQSettingsNewAthlete(root.canonicalPath(), name);
    appsettings->setCValue(name, GC_UPGRADE_FOLDER_SUCCESS, true);
    appsettings->setCValue(name, GC_VERSION_USED, QVariant(VERSION_LATEST));

    QDate dob = QDate::fromString(request.args.value("dob").toString(), Qt::ISODate);
    appsettings->setCValue(name, GC_DOB, dob);
    appsettings->setCValue(name, GC_WEIGHT, request.args.value("weight").toDouble());
    appsettings->setCValue(name, GC_HEIGHT, request.args.value("height").toDouble() / 100.0);
    appsettings->setCValue(name, GC_WBALTAU, 300);
    appsettings->setCValue(name, GC_SEX, request.args.value("sex").toString() == "female" ? 1 : 0);
    appsettings->setCValue(name, GC_BIO, request.args.value("bio").toString());

    int cp = request.args.value("cp").toInt();
    int ftp = request.args.contains("ftp") ? request.args.value("ftp").toInt() : cp;
    Zones zones;
    zones.addZoneRange(dob, cp, 0, ftp, request.args.value("w").toInt(), request.args.value("pmax").toInt());
    zones.write(dirs.config());

    HrZones hrzones;
    hrzones.addHrZoneRange(dob, request.args.value("lthr").toInt(), 0,
                           request.args.value("resthr").toInt(), request.args.value("maxhr").toInt());
    hrzones.write(dirs.config());

    // pace zones from critical velocity in km/h, as the wizard's defaults
    PaceZones runPace(false);
    runPace.addZoneRange(dob, request.args.value("cv-run").toDouble(), 0);
    runPace.write(dirs.config());
    PaceZones swimPace(true);
    swimPace.addZoneRange(dob, request.args.value("cv-swim").toDouble(), 0);
    swimPace.write(dirs.config());

    appsettings->syncQSettingsAllAthletes();

    QJsonObject data;
    data.insert("name", name);
    data.insert("folder", athleteDir.canonicalPath());
    CommandResult result = CommandResult::success(data);
    result.text = QString("created athlete %1 in %2\n").arg(name).arg(athleteDir.canonicalPath());
    return result;
}

static QJsonObject
powerZonesJson(const Zones *zones)
{
    QJsonObject o;
    QJsonArray ranges;
    if (!zones) return o;
    for (int r = 0; r < zones->getRangeSize(); r++) {
        QJsonObject range;
        range.insert("from", zones->getStartDate(r).toString(Qt::ISODate));
        range.insert("cp", zones->getCP(r));
        range.insert("ftp", zones->getFTP(r));
        range.insert("aet", zones->getAeT(r));
        range.insert("wprime", zones->getWprime(r));
        range.insert("pmax", zones->getPmax(r));
        QJsonArray list;
        for (int z = 0; z < zones->numZones(r); z++) {
            QString name, desc;
            int low = 0, high = 0;
            zones->zoneInfo(r, z, name, desc, low, high);
            QJsonObject zone;
            zone.insert("name", name);
            zone.insert("description", desc);
            zone.insert("low", low);
            if (high != INT_MAX) zone.insert("high", high);
            list.append(zone);
        }
        range.insert("zones", list);
        ranges.append(range);
    }
    o.insert("ranges", ranges);
    return o;
}

static QJsonObject
hrZonesJson(const HrZones *zones)
{
    QJsonObject o;
    QJsonArray ranges;
    if (!zones) return o;
    for (int r = 0; r < zones->getRangeSize(); r++) {
        QJsonObject range;
        range.insert("from", zones->getStartDate(r).toString(Qt::ISODate));
        range.insert("lthr", zones->getLT(r));
        range.insert("aet", zones->getAeT(r));
        range.insert("resthr", zones->getRestHr(r));
        range.insert("maxhr", zones->getMaxHr(r));
        QJsonArray list;
        for (int z = 0; z < zones->numZones(r); z++) {
            QString name, desc;
            int low = 0, high = 0;
            double trimp = 0;
            zones->zoneInfo(r, z, name, desc, low, high, trimp);
            QJsonObject zone;
            zone.insert("name", name);
            zone.insert("description", desc);
            zone.insert("low", low);
            if (high != INT_MAX) zone.insert("high", high);
            list.append(zone);
        }
        range.insert("zones", list);
        ranges.append(range);
    }
    o.insert("ranges", ranges);
    return o;
}

static CommandResult
showAthlete(CommandEnvironment &env, const CommandRequest &)
{
    AthleteSession &s = *env.session;
    Athlete *athlete = s.athlete();
    QString name = s.name();

    QJsonObject data;
    data.insert("name", name);
    data.insert("folder", s.folder());
    data.insert("id", athlete->id.toString());
    data.insert("dob", appsettings->cvalue(name, GC_DOB).toDate().toString(Qt::ISODate));
    data.insert("weight", appsettings->cvalue(name, GC_WEIGHT).toDouble());
    data.insert("height", appsettings->cvalue(name, GC_HEIGHT).toDouble() * 100.0);
    data.insert("sex", appsettings->cvalue(name, GC_SEX).toInt() == 0 ? "male" : "female");

    int actual = 0, planned = 0;
    QMap<QString,int> sports;
    RideItem *first = nullptr, *last = nullptr;
    for (RideItem *item : s.rideCache()->rides()) {
        if (item->planned) { planned++; continue; }
        actual++;
        sports[item->sport.isEmpty() ? QString("Unknown") : item->sport]++;
        if (!first) first = item;
        last = item;
    }
    data.insert("activities", actual);
    data.insert("planned", planned);
    QJsonObject bySport;
    for (auto it = sports.constBegin(); it != sports.constEnd(); ++it) bySport.insert(it.key(), it.value());
    data.insert("sports", bySport);
    if (first) data.insert("first", activityStart(first));
    if (last) data.insert("last", activityStart(last));
    data.insert("refreshed", s.refreshedOnOpen());
    return CommandResult::success(data);
}

static CommandResult
refreshAthlete(CommandEnvironment &env, const CommandRequest &request)
{
    AthleteSession &s = *env.session;
    int refreshed = s.refreshedOnOpen();

    if (request.args.value("rebuild").toBool(false)) {
        // make every activity look changed so all metrics and caches are
        // recomputed, as when the cache folder is deleted
        for (RideItem *item : s.rideCache()->rides()) {
            item->crc = 0;
            item->timestamp = 0;
        }
        s.refresh();
        refreshed = s.rideCache()->lastStaleCount();
    }

    QJsonObject data;
    data.insert("refreshed", refreshed);
    data.insert("activities", s.rideCache()->count());
    CommandResult result = CommandResult::success(data);
    result.text = QString("%1 of %2 activities recomputed\n").arg(refreshed).arg(s.rideCache()->count());
    return result;
}

static CommandResult
showZones(CommandEnvironment &env, const CommandRequest &request)
{
    Athlete *athlete = env.session->athlete();
    QString sport = request.args.value("sport").toString();

    QJsonObject data;
    data.insert("sport", sport);
    data.insert("power", powerZonesJson(athlete->zones(sport)));
    data.insert("hr", hrZonesJson(athlete->hrZones(sport)));

    QJsonArray pace;
    for (int swim = 0; swim < 2; swim++) {
        const PaceZones *pz = athlete->paceZones(swim);
        if (!pz) continue;
        for (int r = 0; r < pz->getRangeSize(); r++) {
            QJsonObject range;
            range.insert("sport", swim ? "Swim" : "Run");
            range.insert("from", pz->getStartDate(r).toString(Qt::ISODate));
            range.insert("cv", pz->getCV(r));
            pace.append(range);
        }
    }
    data.insert("pace", pace);
    return CommandResult::success(data);
}

void
registerAthleteCommands(CommandRegistry &registry)
{
    Command list;
    list.spec.name = "athlete.list";
    list.spec.summary = "list the athletes in the athletes folder";
    list.spec.scope = Scope::Global;
    list.spec.httpMethod = "GET";
    list.spec.httpPath = "/athletes";
    list.handler = listAthletes;
    registry.add(list);

    Command create;
    create.spec.name = "athlete.create";
    create.spec.summary = "create a new athlete, as the new athlete wizard";
    create.spec.scope = Scope::Global;
    create.spec.modifies = true;
    create.spec.params << ParamSpec("name", ParamType::String, "athlete name (folder name)").req().pos();
    create.spec.params << ParamSpec("dob", ParamType::Date, "date of birth").def("1980-01-01");
    create.spec.params << ParamSpec("sex", ParamType::String, "sex").def("male").oneOf({ "male", "female" });
    create.spec.params << ParamSpec("weight", ParamType::Double, "weight in kg").def(75.0);
    create.spec.params << ParamSpec("height", ParamType::Double, "height in cm").def(175.0);
    create.spec.params << ParamSpec("bio", ParamType::String, "biography");
    create.spec.params << ParamSpec("cp", ParamType::Int, "critical power in watts").def(250);
    create.spec.params << ParamSpec("ftp", ParamType::Int, "FTP in watts (default: cp)");
    create.spec.params << ParamSpec("w", ParamType::Int, "W' in joules").def(20000);
    create.spec.params << ParamSpec("pmax", ParamType::Int, "maximal power in watts").def(1000);
    create.spec.params << ParamSpec("lthr", ParamType::Int, "lactate threshold heart rate").def(165);
    create.spec.params << ParamSpec("resthr", ParamType::Int, "resting heart rate").def(50);
    create.spec.params << ParamSpec("maxhr", ParamType::Int, "maximum heart rate").def(190);
    create.spec.params << ParamSpec("cv-run", ParamType::Double, "running critical velocity in km/h").def(12.0);
    create.spec.params << ParamSpec("cv-swim", ParamType::Double, "swimming critical velocity in km/h").def(3.0);
    create.spec.httpMethod = "POST";
    create.spec.httpPath = "/athletes";
    create.handler = createAthlete;
    registry.add(create);

    Command show;
    show.spec.name = "athlete.show";
    show.spec.summary = "show the athlete's details and activity counts";
    show.spec.scope = Scope::Athlete;
    show.spec.httpMethod = "GET";
    show.spec.httpPath = "/athletes/{athlete}";
    show.handler = showAthlete;
    registry.add(show);

    Command refresh;
    refresh.spec.name = "athlete.refresh";
    refresh.spec.summary = "bring metrics and critical power up to date with the files on disk";
    refresh.spec.description =
        "Every command already does this when it opens the athlete: activities and\n"
        "zones changed on disk since the last run are recomputed, the same as opening\n"
        "the athlete in the GUI. Use --rebuild to recompute everything.";
    refresh.spec.scope = Scope::Athlete;
    refresh.spec.modifies = true;
    refresh.spec.params << ParamSpec("rebuild", ParamType::Bool, "recompute every activity");
    refresh.spec.httpMethod = "POST";
    refresh.spec.httpPath = "/athletes/{athlete}/refresh";
    refresh.handler = refreshAthlete;
    registry.add(refresh);

    Command zones;
    zones.spec.name = "zones.show";
    zones.spec.summary = "show power, heart rate and pace zones";
    zones.spec.scope = Scope::Athlete;
    zones.spec.params << ParamSpec("sport", ParamType::String, "sport the zones are for").def("Bike");
    zones.spec.httpMethod = "GET";
    zones.spec.httpPath = "/athletes/{athlete}/zones";
    zones.handler = showZones;
    registry.add(zones);
}

} // namespace Headless
