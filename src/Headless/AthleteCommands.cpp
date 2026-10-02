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
#include "ActivityJson.h"
#include "SeasonRange.h"

#include "Context.h"
#include "Athlete.h"
#include "RideCache.h"
#include "RideItem.h"
#include "Settings.h"
#include "GcUpgrade.h"
#include "Zones.h"
#include "HrZones.h"
#include "PaceZones.h"
#include "Measures.h"
#include "RideFile.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <functional>
#include <algorithm>
#include <cmath>

namespace Headless {

static CommandResult
listAthletes(CommandEnvironment &env, const CommandRequest &)
{
    if (!QFileInfo(env.home).isDir())
        return CommandResult::failure(Status::NotFound, HeadlessApp::missingHome(env.home));

    QJsonArray list;
    for (const QString &name : HeadlessApp::athletes(env.home)) {
        QString folder = QDir(env.home).absoluteFilePath(name);
        QJsonObject o;
        o.insert("name", name);
        o.insert("folder", folder);

        // files of the formats GoldenCheetah reads, as it would load them
        int count = 0;
        const QStringList suffixes = RideFileFactory::instance().suffixes();
        for (const QFileInfo &f : QDir(folder + "/activities").entryInfoList(QDir::Files))
            if (suffixes.contains(f.suffix().toLower())) count++;
        o.insert("activity_files", count);

        // is someone else using it? (looked at, not taken)
        QString holder;
        if (AthleteLock::peek(folder, &holder) == AthleteLock::State::InUse) o.insert("in_use_by", holder);
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
    if (!isAthleteName(name))
        return CommandResult::failure(Status::Usage, QString("'%1' is not a valid athlete name").arg(name));

    // the athletes folder may not exist yet
    QDir root(env.home);
    if (!root.exists() && !QDir().mkpath(env.home))
        return CommandResult::failure(Status::Failed, QString("can't create %1").arg(env.home));

    if (!HeadlessApp::isInitialised()) {
        QString error;
        if (!HeadlessApp::initialise(env.home, error))
            return CommandResult::failure(Status::Failed, error);
    }

    if (root.exists(name)) return CommandResult::failure(Status::Failed, QString("athlete '%1' already exists").arg(name));

    // what the new athlete wizard does; a half made athlete is removed again
    NewAthleteDefaults defaults;
    defaults.dob = QDate::fromString(request.args.value("dob").toString(), Qt::ISODate);
    defaults.weight = request.args.value("weight").toDouble();
    defaults.height = request.args.value("height").toDouble() / 100.0;
    defaults.wbaltau = 300;
    defaults.sex = request.args.value("sex").toString() == "female" ? 1 : 0;
    defaults.bio = request.args.value("bio").toString();
    defaults.cp = request.args.value("cp").toInt();
    defaults.ftp = request.args.contains("ftp") ? request.args.value("ftp").toInt() : defaults.cp;
    defaults.wprime = request.args.value("w").toInt();
    defaults.pmax = request.args.value("pmax").toInt();
    defaults.lthr = request.args.value("lthr").toInt();
    defaults.resthr = request.args.value("resthr").toInt();
    defaults.maxhr = request.args.value("maxhr").toInt();
    // pace zones from critical velocity in km/h, as the wizard's defaults
    defaults.cvRun = request.args.value("cv-run").toDouble();
    defaults.cvSwim = request.args.value("cv-swim").toDouble();
    QString error;
    if (!createAthleteFolder(root, name, defaults, true, &error)) return CommandResult::failure(Status::Failed, error);
    QDir athleteDir(root.canonicalPath() + "/" + name);

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

//
// The athlete's About and Model settings, read and written with the keys,
// units and defaults of the GUI's AboutRiderPage and AboutModelPage
//

// the crank lengths the About page offers, in mm
static const QStringList crankLengths = {
    "130", "135", "140", "145", "150", "155", "160", "162.5", "165", "167.5", "170", "172.5",
    "175", "177.5", "180", "182.5", "185", "190", "195", "200", "205", "210", "215", "220"
};

static QString
nickname(const QString &name)
{
    QString nick = appsettings->cvalue(name, GC_NICKNAME, "").toString();
    return nick == "0" ? QString() : nick;      // as the About page shows it
}

// where Athlete::getWeight finds the weight for a day without an activity
static QString
weightSource(Athlete *athlete, const QDate &date)
{
    MeasuresGroup *body = athlete->measures->getGroup(Measures::Body);
    if (body && body->getFieldValue(date)) return "measure";
    if (appsettings->cvalue(athlete->cyclist, GC_WEIGHT, "75.0").toString().toDouble() > 0) return "setting";
    return "default";
}

static void
insertAthleteSettings(QJsonObject &data, Athlete *athlete, const QString &name)
{
    data.insert("nickname", nickname(name));
    data.insert("dob", appsettings->cvalue(name, GC_DOB).toDate().toString(Qt::ISODate));
    data.insert("sex", appsettings->cvalue(name, GC_SEX).toInt() == 0 ? "male" : "female");
    data.insert("weight", appsettings->cvalue(name, GC_WEIGHT).toDouble());
    data.insert("height", appsettings->cvalue(name, GC_HEIGHT).toDouble() * 100.0);
    QDate today = QDate::currentDate();
    data.insert("weight_today", jsonNumber(athlete->getWeight(today)));
    data.insert("weight_source", weightSource(athlete, today));
    data.insert("crank_length", appsettings->cvalue(name, GC_CRANKLENGTH, "175").toString().toDouble());
    data.insert("wheel_size", appsettings->cvalue(name, GC_WHEELSIZE, 2100).toInt());
    data.insert("wbal_tau", appsettings->cvalue(name, GC_WBALTAU, Athlete::defaultWbaltau).toInt());
    data.insert("sts_days", appsettings->cvalue(name, GC_STS_DAYS, Athlete::defaultSTSavg).toInt());
    data.insert("lts_days", appsettings->cvalue(name, GC_LTS_DAYS, Athlete::defaultLTSavg).toInt());
    data.insert("sb_today", appsettings->cvalue(name, GC_SB_TODAY, Athlete::defaultSBToday).toInt() != 0);
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
    insertAthleteSettings(data, athlete, name);

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

// settings changed: written now (a folder that can't be written fails here,
// and the old values are put back), then the dialog's notifications
static CommandResult
saveAthleteSettings(AthleteSession &s, const QList<QPair<QString, QVariant>> &values, qint32 changed, int &refreshed)
{
    QString name = s.name();
    QList<QPair<QString, QVariant>> before;
    for (const auto &value : values) {
        before << qMakePair(value.first, appsettings->cvalue(name, value.first, QVariant()));
        appsettings->setCValue(name, value.first, value.second);
    }
    QString error;
    for (const auto &value : values) {
        if (appsettings->syncCValue(name, value.first, &error)) continue;
        for (const auto &old : before) if (old.second.isValid()) appsettings->setCValue(name, old.first, old.second);
        return CommandResult::failure(Status::Failed, error);
    }

    // as AthleteConfigDialog: weight, height, wheel and crank changes
    // recompute what depends on them (RideCache::configChanged)
    refreshed = 0;
    if (changed) {
        s.rideCache()->cancel();
        s.context()->notifyConfigChanged(changed);
        if (changed & (CONFIG_ATHLETE | CONFIG_GENERAL | CONFIG_ZONES)) {
            s.waitForRefresh();
            refreshed = s.rideCache()->lastStaleCount();
        }
    }
    return CommandResult::success();
}

static CommandResult
setAthlete(CommandEnvironment &env, const CommandRequest &request)
{
    AthleteSession &s = *env.session;
    QString name = s.name();
    auto has = [&](const char *k) { return request.args.contains(k); };
    auto num = [&](const char *k) { return request.args.value(k).toDouble(); };
    auto range = [](const char *what, double v, double low, double high, const QString &units) {
        if (v >= low && v <= high) return QString();
        return QString("%1 must be %2 to %3%4, got %5").arg(what).arg(low).arg(high).arg(units).arg(v);
    };

    // check everything before anything is written
    QStringList bad;
    if (has("weight")) bad << range("--weight", num("weight"), 0, 999.9, " kg");
    if (has("height")) bad << range("--height", num("height"), 0, 999.9, " cm");
    if (has("wheel-size")) bad << range("--wheel-size", num("wheel-size"), 1, 9999, " mm");
    if (has("wbal-tau")) bad << range("--wbal-tau", num("wbal-tau"), 30, 1200, " s");
    if (has("sts-days")) bad << range("--sts-days", num("sts-days"), 1, 21, " days");
    if (has("lts-days")) bad << range("--lts-days", num("lts-days"), 7, 56, " days");
    QString crank;
    if (has("crank-length")) {
        for (const QString &c : crankLengths) if (c.toDouble() == num("crank-length")) crank = c;
        if (crank.isEmpty())
            bad << QString("--crank-length must be one of %1 (mm), got %2").arg(crankLengths.join(", ")).arg(num("crank-length"));
    }
    bad.removeAll(QString());
    if (!bad.isEmpty()) return CommandResult::failure(Status::Usage, bad.join("; "));

    // written as the About and Model pages write them
    QList<QPair<QString, QVariant>> values;
    qint32 changed = 0;
    if (has("nickname")) values << qMakePair(QString(GC_NICKNAME), QVariant(request.args.value("nickname").toString()));
    if (has("dob")) values << qMakePair(QString(GC_DOB), QVariant(QDate::fromString(request.args.value("dob").toString(), Qt::ISODate)));
    if (has("sex")) values << qMakePair(QString(GC_SEX), QVariant(request.args.value("sex").toString() == "female" ? 1 : 0));
    if (has("weight")) {
        // the page's spin box keeps one decimal
        double weight = qRound(num("weight") * 10.0) / 10.0;
        if (weight != appsettings->cvalue(name, GC_WEIGHT).toDouble()) changed |= CONFIG_ATHLETE;
        values << qMakePair(QString(GC_WEIGHT), QVariant(weight));
    }
    if (has("height")) {
        double height = qRound(num("height") * 10.0) / 10.0 / 100.0;
        if (height != appsettings->cvalue(name, GC_HEIGHT).toDouble()) changed |= CONFIG_ATHLETE;
        values << qMakePair(QString(GC_HEIGHT), QVariant(height));
    }
    if (has("crank-length")) {
        if (crank != appsettings->cvalue(name, GC_CRANKLENGTH, "175").toString()) changed |= CONFIG_GENERAL;
        values << qMakePair(QString(GC_CRANKLENGTH), QVariant(crank));
    }
    if (has("wheel-size")) {
        int wheel = request.args.value("wheel-size").toInt();
        if (wheel != appsettings->cvalue(name, GC_WHEELSIZE, 2100).toInt()) changed |= CONFIG_GENERAL;
        values << qMakePair(QString(GC_WHEELSIZE), QVariant(wheel));
    }
    // W'bal tau is only used by Train's real time W'bal: nothing recomputes, as in the GUI
    if (has("wbal-tau")) values << qMakePair(QString(GC_WBALTAU), QVariant(request.args.value("wbal-tau").toInt()));
    if (has("sts-days")) {
        int sts = request.args.value("sts-days").toInt();
        if (sts != appsettings->cvalue(name, GC_STS_DAYS, Athlete::defaultSTSavg).toInt()) changed |= CONFIG_PMC;
        values << qMakePair(QString(GC_STS_DAYS), QVariant(sts));
    }
    if (has("lts-days")) {
        int lts = request.args.value("lts-days").toInt();
        if (lts != appsettings->cvalue(name, GC_LTS_DAYS, Athlete::defaultLTSavg).toInt()) changed |= CONFIG_PMC;
        values << qMakePair(QString(GC_LTS_DAYS), QVariant(lts));
    }
    if (has("sb-today")) values << qMakePair(QString(GC_SB_TODAY), QVariant(request.args.value("sb-today").toString() == "true" ? 1 : 0));

    if (values.isEmpty())
        return CommandResult::failure(Status::Usage, "give a setting to change: --nickname, --dob, --sex, --height, --weight, "
                                                     "--crank-length, --wheel-size, --wbal-tau, --sts-days, --lts-days or --sb-today");

    int refreshed = 0;
    CommandResult saved = saveAthleteSettings(s, values, changed, refreshed);
    if (!saved.ok()) return saved;

    QJsonObject data;
    data.insert("name", name);
    insertAthleteSettings(data, s.athlete(), name);
    data.insert("refreshed", refreshed);
    CommandResult result = CommandResult::success(data);
    result.text = QString("%1 setting%2 saved for %3, %4 activities recomputed\n")
                  .arg(values.count()).arg(values.count() == 1 ? "" : "s").arg(name).arg(refreshed);
    return result;
}

static CommandResult
refreshAthlete(CommandEnvironment &env, const CommandRequest &request)
{
    AthleteSession &s = *env.session;
    int refreshed = s.refreshedOnOpen();

    if (request.args.value("rebuild").toBool(false)) {
        // every activity's metrics recomputed, marked stale as the core
        // does for a changed one (its .cpx is rebuilt when out of date
        // with the file, as always)
        for (RideItem *item : s.rideCache()->rides()) item->isstale = true;
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

// the power zones page's per sport options: CP model and use CP for FTP
static const QStringList cpModels = { "manual", "cp2", "cp3", "ext" };
static const QStringList cogganMetrics = { "cp", "ftp" };

static void
insertPowerOptions(QJsonObject &power, const QString &name, const Zones *zones)
{
    if (!zones) return;
    int model = appsettings->cvalue(name, zones->useCPModelSetting(), 0).toInt();
    power.insert("cp_model", cpModels.value(model, cpModels.first()));
    power.insert("coggan_metrics", appsettings->cvalue(name, zones->useCPforFTPSetting(), 0).toInt() ? "ftp" : "cp");
}

static CommandResult
showZones(CommandEnvironment &env, const CommandRequest &request)
{
    Athlete *athlete = env.session->athlete();
    QString sport = request.args.value("sport").toString();
    bool metricUnits = !request.args.value("imperial").toBool(false);

    QJsonObject data;
    data.insert("sport", sport);
    QJsonObject power = powerZonesJson(athlete->zones(sport));
    insertPowerOptions(power, env.session->name(), athlete->zones(sport));
    data.insert("power", power);
    data.insert("hr", hrZonesJson(athlete->hrZones(sport)));

    QJsonArray pace;
    for (int swim = 0; swim < 2; swim++) {
        const PaceZones *pz = athlete->paceZones(swim);
        if (!pz) continue;
        for (int r = 0; r < pz->getRangeSize(); r++) {
            QJsonObject range;
            range.insert("sport", swim ? "Swim" : "Run");
            range.insert("from", pz->getStartDate(r).toString(Qt::ISODate));
            range.insert("cv", jsonNumber(pz->getCV(r)));
            range.insert("cv_pace", pz->kphToPaceString(pz->getCV(r), metricUnits));
            range.insert("units", pz->paceUnits(metricUnits));
            // bounds in km/h, and as a pace the way the GUI shows them
            QJsonArray list;
            for (int z = 0; z < pz->numZones(r); z++) {
                QString name, desc;
                double low, high;
                pz->zoneInfo(r, z, name, desc, low, high);
                QJsonObject zone;
                zone.insert("name", name);
                zone.insert("description", desc);
                zone.insert("low", jsonNumber(low));
                zone.insert("low_pace", low > 0 ? QJsonValue(pz->kphToPaceString(low, metricUnits)) : QJsonValue());
                if (high < INT_MAX) {
                    zone.insert("high", jsonNumber(high));
                    zone.insert("high_pace", pz->kphToPaceString(high, metricUnits));
                }
                list.append(zone);
            }
            range.insert("zones", list);
            pace.append(range);
        }
    }
    data.insert("pace", pace);
    return CommandResult::success(data);
}

// the GUI's zone pages re-read the file after writing and tell everyone
static void
zonesChanged(AthleteSession &s)
{
    s.context()->notifyConfigChanged(CONFIG_ZONES);
    s.waitForRefresh();
}

// written, then read back, which also undoes the change when it wasn't saved
template <class Z>
static bool
saveZones(Z *zones, Athlete *athlete, QString &error)
{
    bool saved = zones->write(athlete->home->config(), &error);
    QFile file(athlete->home->config().canonicalPath() + "/" + zones->fileName());
    zones->read(file);
    return saved;
}

// the GUI's zone pages edit a sport's own file; without one the sport uses
// Bike's zones, and the page shows no ranges
template <class Z>
static bool
ownZonesFile(const Z *zones, Athlete *athlete)
{
    return QFileInfo::exists(athlete->home->config().canonicalPath() + "/" + zones->fileName());
}

// the range covering this date, whose anchors a new range copies
static int
coveringRange(int count, const std::function<QDate(int)> &start, const std::function<int(QDate)> &which, const QDate &from)
{
    int found = which(from);
    if (found >= 0) return found;
    int best = -1;
    for (int r = 0; r < count; r++) if (start(r) < from) best = r;
    return best;
}

static CommandResult
setZones(CommandEnvironment &env, const CommandRequest &request)
{
    AthleteSession &s = *env.session;
    Athlete *athlete = s.athlete();
    QString sport = RideFile::sportTag(request.args.value("sport").toString());
    QDate from = QDate::fromString(request.args.value("from").toString(), Qt::ISODate);
    QString type = request.args.value("type").toString();
    QString error;
    QJsonObject data;
    data.insert("sport", sport);
    data.insert("from", from.toString(Qt::ISODate));

    auto has = [&](const char *k) { return request.args.contains(k); };
    auto val = [&](const char *k) { return int(request.args.value(k).toInt()); };

    if (type == "power") {
        if (!athlete->zones_.contains(sport)) return CommandResult::failure(Status::NotFound, QString("no power zones for sport '%1'").arg(sport));
        Zones *zones = athlete->zones_.value(sport);
        int range = -1;
        for (int r = 0; r < zones->getRangeSize(); r++) if (zones->getStartDate(r) == from) range = r;
        bool added = range < 0;
        if (added) {
            int source = coveringRange(zones->getRangeSize(),
                                        [&](int r) { return zones->getStartDate(r); },
                                        [&](QDate d) { return zones->whichRange(d); }, from);
            if (!has("cp") && source < 0) return CommandResult::failure(Status::Usage, "power zones need --cp");
            int cp = has("cp") ? val("cp") : zones->getCP(source);
            int aet = has("aet") ? val("aet") : (source >= 0 ? zones->getAeT(source) : 0);
            int ftp = has("ftp") ? val("ftp") : (source >= 0 ? zones->getFTP(source) : cp);
            int w = has("w") ? val("w") : (source >= 0 ? zones->getWprime(source) : 20000);
            int pmax = has("pmax") ? val("pmax") : (source >= 0 ? zones->getPmax(source) : 1000);
            range = zones->addZoneRange(from, cp, aet, ftp, w, pmax);
        } else {
            if (!has("cp") && !has("ftp") && !has("aet") && !has("w") && !has("pmax"))
                return CommandResult::failure(Status::Usage, "give a value to set: --cp, --ftp, --w, --pmax or --aet");
            if (has("cp")) zones->setCP(range, val("cp"));
            if (has("ftp")) zones->setFTP(range, val("ftp"));
            if (has("aet")) zones->setAeT(range, val("aet"));
            if (has("w")) zones->setWprime(range, val("w"));
            if (has("pmax")) zones->setPmax(range, val("pmax"));
            zones->setZonesFromCP(range);
        }
        if (!saveZones(zones, athlete, error)) return CommandResult::failure(Status::Failed, error);
        data.insert("status", added ? "added" : "updated");
        data.insert("power", powerZonesJson(zones));

    } else if (type == "hr") {
        if (!athlete->hrzones_.contains(sport)) return CommandResult::failure(Status::NotFound, QString("no heart rate zones for sport '%1'").arg(sport));
        HrZones *zones = athlete->hrzones_.value(sport);
        int range = -1;
        for (int r = 0; r < zones->getRangeSize(); r++) if (zones->getStartDate(r) == from) range = r;
        bool added = range < 0;
        if (added) {
            int source = coveringRange(zones->getRangeSize(),
                                        [&](int r) { return zones->getStartDate(r); },
                                        [&](QDate d) { return zones->whichRange(d); }, from);
            if (!has("lthr") && source < 0) return CommandResult::failure(Status::Usage, "heart rate zones need --lthr");
            int lthr = has("lthr") ? val("lthr") : zones->getLT(source);
            int aet = has("aet") ? val("aet") : (source >= 0 ? zones->getAeT(source) : 0);
            int rest = has("resthr") ? val("resthr") : (source >= 0 ? zones->getRestHr(source) : 50);
            int maxhr = has("maxhr") ? val("maxhr") : (source >= 0 ? zones->getMaxHr(source) : 190);
            range = zones->addHrZoneRange(from, lthr, aet, rest, maxhr);
        } else {
            if (!has("lthr") && !has("aet") && !has("resthr") && !has("maxhr"))
                return CommandResult::failure(Status::Usage, "give a value to set: --lthr, --aet, --resthr or --maxhr");
            if (has("lthr")) zones->setLT(range, val("lthr"));
            if (has("aet")) zones->setAeT(range, val("aet"));
            if (has("resthr")) zones->setRestHr(range, val("resthr"));
            if (has("maxhr")) zones->setMaxHr(range, val("maxhr"));
            zones->setHrZonesFromLT(range);
        }
        if (!saveZones(zones, athlete, error)) return CommandResult::failure(Status::Failed, error);
        data.insert("status", added ? "added" : "updated");
        data.insert("hr", hrZonesJson(zones));

    } else {
        bool swim = sport == "Swim";
        if (sport != "Run" && !swim)
            return CommandResult::failure(Status::Usage, "pace zones are for Run or Swim, set --sport");
        PaceZones *zones = athlete->pacezones_[swim ? 1 : 0];
        if (!zones) return CommandResult::failure(Status::NotFound, QString("no pace zones for %1").arg(sport));
        int range = -1;
        for (int r = 0; r < zones->getRangeSize(); r++) if (zones->getStartDate(r) == from) range = r;
        bool added = range < 0;
        if (added) {
            int source = coveringRange(zones->getRangeSize(),
                                        [&](int r) { return zones->getStartDate(r); },
                                        [&](QDate d) { return zones->whichRange(d); }, from);
            if (!has("cv") && source < 0) return CommandResult::failure(Status::Usage, "pace zones need --cv");
            double cv = has("cv") ? request.args.value("cv").toDouble() : zones->getCV(source);
            double aet = has("aet") ? val("aet") : (source >= 0 ? zones->getAeT(source) : 0);
            range = zones->addZoneRange(from, cv, aet);
        } else {
            if (!has("cv") && !has("aet"))
                return CommandResult::failure(Status::Usage, "give a value to set: --cv or --aet");
            if (has("cv")) zones->setCV(range, request.args.value("cv").toDouble());
            if (has("aet")) zones->setAeT(range, val("aet"));
            zones->setZonesFromCV(range);
        }
        if (!saveZones(zones, athlete, error)) return CommandResult::failure(Status::Failed, error);
        data.insert("status", added ? "added" : "updated");
        data.insert("cv", jsonNumber(zones->getCV(range)));
    }

    zonesChanged(s);
    data.insert("refreshed", s.rideCache()->lastStaleCount());
    CommandResult result = CommandResult::success(data);
    result.text = QString("%1 %2 zones for %3 from %4, %5 activities recomputed\n")
                  .arg(data.value("status").toString()).arg(type).arg(sport).arg(from.toString(Qt::ISODate))
                  .arg(s.rideCache()->lastStaleCount());
    return result;
}

// a range removed as the Delete button under a zones page's ranges does
template <class Z>
static CommandResult
removeRange(Athlete *athlete, Z *zones, const QString &what, const QString &sport, bool usesBike,
            const QDate &from, QJsonArray &remaining)
{
    if (!ownZonesFile(zones, athlete))
        return CommandResult::failure(Status::NotFound, QString("%1 has no %2 zones of its own%3")
                                      .arg(sport).arg(what).arg(usesBike ? ", it uses Bike's" : ""));
    int range = -1;
    QStringList starts;
    for (int r = 0; r < zones->getRangeSize(); r++) {
        if (zones->getStartDate(r) == from) range = r;
        starts << zones->getStartDate(r).toString(Qt::ISODate);
    }
    if (range < 0)
        return CommandResult::failure(Status::NotFound, QString("no %1 zone range for %2 starts on %3 (ranges start on %4)")
                                      .arg(what).arg(sport).arg(from.toString(Qt::ISODate)).arg(starts.join(", ")));
    if (zones->getRangeSize() == 1)
        return CommandResult::failure(Status::Usage, QString("the range from %1 is the only %2 zone range for %3 and can't be "
                                      "removed; change it with zones set").arg(from.toString(Qt::ISODate)).arg(what).arg(sport));

    // the previous range is extended to cover the removed one
    zones->deleteRange(range);
    QString error;
    if (!saveZones(zones, athlete, error)) return CommandResult::failure(Status::Failed, error);
    for (int r = 0; r < zones->getRangeSize(); r++) remaining.append(zones->getStartDate(r).toString(Qt::ISODate));
    return CommandResult::success();
}

static CommandResult
removeZones(CommandEnvironment &env, const CommandRequest &request)
{
    AthleteSession &s = *env.session;
    Athlete *athlete = s.athlete();
    QString sport = RideFile::sportTag(request.args.value("sport").toString());
    QDate from = QDate::fromString(request.args.value("from").toString(), Qt::ISODate);
    QString type = request.args.value("type").toString();

    QJsonArray remaining;
    CommandResult removed;
    if (type == "power") {
        if (!athlete->zones_.contains(sport)) return CommandResult::failure(Status::NotFound, QString("no power zones for sport '%1'").arg(sport));
        removed = removeRange(athlete, athlete->zones_.value(sport), "power", sport, sport != "Bike", from, remaining);
    } else if (type == "hr") {
        if (!athlete->hrzones_.contains(sport)) return CommandResult::failure(Status::NotFound, QString("no heart rate zones for sport '%1'").arg(sport));
        removed = removeRange(athlete, athlete->hrzones_.value(sport), "heart rate", sport, sport != "Bike", from, remaining);
    } else {
        if (sport != "Run" && sport != "Swim") return CommandResult::failure(Status::Usage, "pace zones are for Run or Swim, set --sport");
        PaceZones *zones = athlete->pacezones_[sport == "Swim" ? 1 : 0];
        if (!zones) return CommandResult::failure(Status::NotFound, QString("no pace zones for %1").arg(sport));
        removed = removeRange(athlete, zones, "pace", sport, false, from, remaining);
    }
    if (!removed.ok()) return removed;

    zonesChanged(s);
    QJsonObject data;
    data.insert("type", type);
    data.insert("sport", sport);
    data.insert("from", from.toString(Qt::ISODate));
    data.insert("status", "removed");
    data.insert("ranges", remaining);
    data.insert("refreshed", s.rideCache()->lastStaleCount());
    CommandResult result = CommandResult::success(data);
    result.text = QString("removed the %1 zones for %2 from %3, %4 activities recomputed\n")
                  .arg(type).arg(sport).arg(from.toString(Qt::ISODate)).arg(s.rideCache()->lastStaleCount());
    return result;
}

static CommandResult
setZoneOptions(CommandEnvironment &env, const CommandRequest &request)
{
    AthleteSession &s = *env.session;
    QString name = s.name();
    QString sport = RideFile::sportTag(request.args.value("sport").toString());
    if (!s.athlete()->zones_.contains(sport))
        return CommandResult::failure(Status::NotFound, QString("no power zones for sport '%1'").arg(sport));
    const Zones *zones = s.athlete()->zones_.value(sport);
    if (!request.args.contains("cp-model") && !request.args.contains("coggan-metrics"))
        return CommandResult::failure(Status::Usage, "give an option to set: --cp-model or --coggan-metrics");

    // stored as the page's combo boxes store them, by index
    QList<QPair<QString, QVariant>> values;
    qint32 changed = 0;
    if (request.args.contains("cp-model")) {
        int model = cpModels.indexOf(request.args.value("cp-model").toString());
        if (model != appsettings->cvalue(name, zones->useCPModelSetting(), 0).toInt()) changed = CONFIG_ZONES;
        values << qMakePair(zones->useCPModelSetting(), QVariant(model));
    }
    if (request.args.contains("coggan-metrics")) {
        int ftp = request.args.value("coggan-metrics").toString() == "ftp" ? 1 : 0;
        if (ftp != (appsettings->cvalue(name, zones->useCPforFTPSetting(), 0).toInt() ? 1 : 0)) changed = CONFIG_ZONES;
        values << qMakePair(zones->useCPforFTPSetting(), QVariant(ftp));
    }
    int refreshed = 0;
    CommandResult saved = saveAthleteSettings(s, values, changed, refreshed);
    if (!saved.ok()) return saved;

    QJsonObject data;
    data.insert("sport", sport);
    insertPowerOptions(data, name, zones);
    data.insert("refreshed", refreshed);
    CommandResult result = CommandResult::success(data);
    result.text = QString("%1 power zones: CP model %2, Coggan metrics use %3, %4 activities recomputed\n")
                  .arg(sport).arg(data.value("cp_model").toString()).arg(data.value("coggan_metrics").toString().toUpper())
                  .arg(refreshed);
    return result;
}

//
// Zone schemes: the Default tab of each zones page. A range whose zones
// come from the default (the GUI's "Def" button, and every range the CLI
// adds) follows the scheme; a range with zones of its own keeps them.
//

static QJsonArray
schemeJson(const QList<QString> &names, const QList<QString> &descs, const QList<int> &lows, const QList<double> *trimps)
{
    QJsonArray list;
    for (int z = 0; z < names.count() && z < lows.count(); z++) {
        QJsonObject zone;
        zone.insert("name", names.at(z));
        zone.insert("description", descs.value(z));
        zone.insert("percent", lows.at(z));
        if (trimps) zone.insert("trimp_k", jsonNumber(trimps->value(z)));
        list.append(zone);
    }
    return list;
}

// where the scheme applies: the zones of that type and sport
struct SchemeTarget {
    Zones *power = nullptr;
    HrZones *hr = nullptr;
    PaceZones *pace = nullptr;
    QString of;             // what the percentages are of
    bool own = true;        // the sport has its own file (else it uses Bike's)
};

static CommandResult
schemeTarget(Athlete *athlete, const QString &type, const QString &sport, SchemeTarget &t)
{
    if (type == "power") {
        if (!athlete->zones_.contains(sport)) return CommandResult::failure(Status::NotFound, QString("no power zones for sport '%1'").arg(sport));
        t.power = athlete->zones_.value(sport);
        t.of = "CP";
        t.own = sport == "Bike" || ownZonesFile(t.power, athlete);
    } else if (type == "hr") {
        if (!athlete->hrzones_.contains(sport)) return CommandResult::failure(Status::NotFound, QString("no heart rate zones for sport '%1'").arg(sport));
        t.hr = athlete->hrzones_.value(sport);
        t.of = "LT";
        t.own = sport == "Bike" || ownZonesFile(t.hr, athlete);
    } else {
        if (sport != "Run" && sport != "Swim") return CommandResult::failure(Status::Usage, "pace zones are for Run or Swim, set --sport");
        t.pace = athlete->pacezones_[sport == "Swim" ? 1 : 0];
        if (!t.pace) return CommandResult::failure(Status::NotFound, QString("no pace zones for %1").arg(sport));
        t.of = "CV";
    }
    return CommandResult::success();
}

static QJsonObject
schemeData(const SchemeTarget &t, const QString &type, const QString &sport)
{
    QJsonObject data;
    data.insert("type", type);
    data.insert("sport", sport);
    data.insert("of", t.of);
    if (t.power) {
        ZoneScheme s = t.power->getScheme();
        data.insert("zones", schemeJson(s.zone_default_name, s.zone_default_desc, s.zone_default, nullptr));
    } else if (t.hr) {
        HrZoneScheme s = t.hr->getScheme();
        data.insert("zones", schemeJson(s.zone_default_name, s.zone_default_desc, s.zone_default, &s.zone_default_trimp));
    } else {
        PaceZoneScheme s = t.pace->getScheme();
        data.insert("zones", schemeJson(s.zone_default_name, s.zone_default_desc, s.zone_default, nullptr));
    }
    if (!t.own) data.insert("uses", "Bike");
    return data;
}

static CommandResult
showScheme(CommandEnvironment &env, const CommandRequest &request)
{
    QString type = request.args.value("type").toString();
    QString sport = RideFile::sportTag(request.args.value("sport").toString());
    SchemeTarget t;
    CommandResult found = schemeTarget(env.session->athlete(), type, sport, t);
    if (!found.ok()) return found;
    return CommandResult::success(schemeData(t, type, sport));
}

static CommandResult
setScheme(CommandEnvironment &env, const CommandRequest &request)
{
    AthleteSession &s = *env.session;
    Athlete *athlete = s.athlete();
    QString type = request.args.value("type").toString();
    QString sport = RideFile::sportTag(request.args.value("sport").toString());
    SchemeTarget t;
    CommandResult found = schemeTarget(athlete, type, sport, t);
    if (!found.ok()) return found;
    if (!t.own)
        return CommandResult::failure(Status::NotFound, QString("%1 has no %2 zones of its own, it uses Bike's; add a range "
                                      "with zones set --sport %1 first").arg(sport).arg(type == "hr" ? "heart rate" : type));

    // NAME,DESCRIPTION,PERCENT (and TRIMPK for heart rate), as the zones file has them
    bool hr = type == "hr";
    QString expected = hr ? "NAME,DESCRIPTION,PERCENT,TRIMPK" : "NAME,DESCRIPTION,PERCENT";
    QList<HrZoneSchemeRow> rows;
    for (const QJsonValue &v : request.args.value("zone").toArray()) {
        QString text = v.toString();
        QStringList parts = text.split(",");
        if (parts.count() != (hr ? 4 : 3))
            return CommandResult::failure(Status::Usage, QString("--zone must be %1, got '%2'").arg(expected).arg(text));
        HrZoneSchemeRow row;
        row.name = parts.at(0).trimmed();
        row.desc = parts.at(1).trimmed();
        QString percent = parts.at(2).trimmed();
        if (percent.endsWith("%")) percent.chop(1);
        bool ok = false;
        row.lo = percent.toInt(&ok);
        if (row.name.isEmpty() || row.desc.isEmpty() || row.name.contains('#') || row.desc.contains('#'))
            return CommandResult::failure(Status::Usage, QString("a zone needs a name and a description, without '#', got '%1'").arg(text));
        if (!ok || row.lo < 0 || row.lo > 1000)
            return CommandResult::failure(Status::Usage, QString("PERCENT must be a whole number from 0 to 1000, got '%1'").arg(parts.at(2)));
        if (hr) {
            row.trimp = parts.at(3).trimmed().toDouble(&ok);
            if (!ok || !std::isfinite(row.trimp) || row.trimp < 0 || row.trimp > 10)
                return CommandResult::failure(Status::Usage, QString("TRIMPK must be a number from 0 to 10, got '%1'").arg(parts.at(3)));
        }
        rows << row;
    }
    if (rows.isEmpty()) return CommandResult::failure(Status::Usage, QString("give the zones, each as --zone %1").arg(expected));

    QString error;
    bool saved;
    if (t.power) {
        QList<ZoneSchemeRow> list;
        for (const HrZoneSchemeRow &r : rows) { ZoneSchemeRow z; z.name = r.name; z.desc = r.desc; z.lo = r.lo; list << z; }
        t.power->setScheme(Zones::schemeFromRows(list));
        saved = saveZones(t.power, athlete, error);
    } else if (t.hr) {
        t.hr->setScheme(HrZones::schemeFromRows(rows));
        saved = saveZones(t.hr, athlete, error);
    } else {
        QList<PaceZoneSchemeRow> list;
        for (const HrZoneSchemeRow &r : rows) { PaceZoneSchemeRow z; z.name = r.name; z.desc = r.desc; z.lo = r.lo; list << z; }
        t.pace->setScheme(PaceZones::schemeFromRows(list));
        saved = saveZones(t.pace, athlete, error);
    }
    if (!saved) return CommandResult::failure(Status::Failed, error);

    zonesChanged(s);
    QJsonObject data = schemeData(t, type, sport);
    data.insert("refreshed", s.rideCache()->lastStaleCount());
    CommandResult result = CommandResult::success(data);
    result.text = QString("%1 %2 zones set for %3, %4 activities recomputed\n")
                  .arg(rows.count()).arg(type).arg(sport).arg(s.rideCache()->lastStaleCount());
    return result;
}

static MeasuresGroup *
measuresGroup(Athlete *athlete, const QString &name)
{
    for (MeasuresGroup *g : athlete->measures->getGroups())
        if (g->getSymbol().compare(name, Qt::CaseInsensitive) == 0 || g->getName().compare(name, Qt::CaseInsensitive) == 0) return g;
    return nullptr;
}

static QString
measureSource(const Measure &m)
{
    switch (m.source) {
    case Measure::Withings: return "withings";
    case Measure::CSV: return "csv";
    case Measure::Tredict: return "tredict";
    default: return "manual";
    }
}

// FIELD=VALUE pairs into a reading's values, in the range the GUI's measures
// editors allow
static CommandResult
measureValues(MeasuresGroup *g, const QJsonValue &set, Measure &m)
{
    QStringList symbols = g->getFieldSymbols();
    QList<QPair<QString, QString>> pairs;
    QString bad;
    bool parsed = parseAssignments(set, pairs, bad);
    for (const auto &pair : pairs) {
        int field = symbols.indexOf(pair.first);
        if (field < 0 || field >= MAX_MEASURES) {
            bad = pair.first + "=" + pair.second;
            parsed = false;
            break;
        }
        bool ok = false;
        double value = pair.second.toDouble(&ok);
        if (!ok || !std::isfinite(value)) return CommandResult::failure(Status::Usage, QString("'%1' is not a number").arg(pair.second));
        if (value < 0 || value > 9999.99)
            return CommandResult::failure(Status::Usage, QString("%1 must be 0 to 9999.99, got %2").arg(pair.first).arg(pair.second));
        m.values[field] = value;
    }
    if (!parsed)
        return CommandResult::failure(Status::Usage, QString("expected FIELD=VALUE with FIELD one of %1, got '%2'").arg(symbols.join(", ")).arg(bad));
    return CommandResult::success();
}

// the reading --when names: that time exactly, or a date with only one reading on it
static int
findMeasure(const QList<Measure> &list, const QString &when, CommandResult &failure)
{
    static const QRegularExpression dateOnly("^\\d{4}-\\d{2}-\\d{2}$");
    if (dateOnly.match(when).hasMatch()) {
        QDate day = QDate::fromString(when, Qt::ISODate);
        if (!day.isValid()) {
            failure = CommandResult::failure(Status::Usage, QString("'%1' is not a date (yyyy-mm-dd)").arg(when));
            return -1;
        }
        QList<int> found;
        QStringList times;
        for (int i = 0; i < list.count(); i++)
            if (list.at(i).when.date() == day) { found << i; times << list.at(i).when.toString(Qt::ISODate); }
        if (found.count() == 1) return found.first();
        if (found.isEmpty()) failure = CommandResult::failure(Status::NotFound, QString("no reading on %1").arg(when));
        else failure = CommandResult::failure(Status::Usage, QString("%1 readings on %2, give the time: %3")
                                              .arg(found.count()).arg(when).arg(times.join(", ")));
        return -1;
    }
    QDateTime time = QDateTime::fromString(when, Qt::ISODate);
    if (!time.isValid()) {
        failure = CommandResult::failure(Status::Usage, "--when must be a date or date and time (yyyy-mm-ddThh:mm:ss)");
        return -1;
    }
    for (int i = 0; i < list.count(); i++) if (list.at(i).when == time) return i;
    failure = CommandResult::failure(Status::NotFound, QString("no reading at %1").arg(time.toString(Qt::ISODate)));
    return -1;
}

// the group written with this list of readings, or left as it was
static CommandResult
saveMeasures(AthleteSession &s, MeasuresGroup *g, QList<Measure> list)
{
    QList<Measure> before = g->measures();
    std::sort(list.begin(), list.end());
    g->setMeasures(list);
    QString error;
    if (!g->write(&error)) {
        g->setMeasures(before);
        return CommandResult::failure(Status::Failed, error);
    }
    // weight feeds per kg metrics: recompute what changed
    s.refresh();
    return CommandResult::success();
}

static CommandResult
listMeasures(CommandEnvironment &env, const CommandRequest &request)
{
    Athlete *athlete = env.session->athlete();
    QString group = request.args.value("group").toString();
    QDate from, to;
    QString error;
    Status status = Status::Ok;
    if (!dateRangeArgs(*env.session, request.args, from, to, error, status)) return CommandResult::failure(status, error);

    QJsonArray groups;
    for (MeasuresGroup *g : athlete->measures->getGroups()) {
        if (!group.isEmpty() && g->getSymbol().compare(group, Qt::CaseInsensitive) != 0
            && g->getName().compare(group, Qt::CaseInsensitive) != 0) continue;
        QJsonObject go;
        go.insert("group", g->getSymbol());
        go.insert("name", g->getName());
        go.insert("fields", QJsonArray::fromStringList(g->getFieldSymbols()));
        QJsonArray rows;
        for (const Measure &m : g->measures()) {
            if (from.isValid() && m.when.date() < from) continue;
            if (to.isValid() && m.when.date() > to) continue;
            QJsonObject row;
            row.insert("when", m.when.toString(Qt::ISODate));
            QStringList symbols = g->getFieldSymbols();
            for (int i = 0; i < symbols.count() && i < MAX_MEASURES; i++) row.insert(symbols.at(i), m.values[i]);
            if (!m.comment.isEmpty()) row.insert("comment", m.comment);
            row.insert("source", measureSource(m));
            rows.append(row);
        }
        go.insert("measures", rows);
        groups.append(go);
    }
    if (!group.isEmpty() && groups.isEmpty()) return CommandResult::failure(Status::NotFound, QString("no measures group '%1'").arg(group));

    // one group asked for: just that group
    if (!group.isEmpty()) return CommandResult::success(groups.first().toObject());
    QJsonObject data;
    data.insert("groups", groups);
    return CommandResult::success(data);
}

static CommandResult
addMeasure(CommandEnvironment &env, const CommandRequest &request)
{
    AthleteSession &s = *env.session;
    MeasuresGroup *g = measuresGroup(s.athlete(), request.args.value("group").toString());
    if (!g) return CommandResult::failure(Status::NotFound, QString("no measures group '%1'").arg(request.args.value("group").toString()));

    QDateTime when = QDateTime::fromString(request.args.value("when").toString(), Qt::ISODate);
    if (!when.isValid()) {
        QDate d = QDate::fromString(request.args.value("when").toString(), Qt::ISODate);
        if (d.isValid()) when = QDateTime(d, QTime(0, 0));
    }
    if (!when.isValid()) return CommandResult::failure(Status::Usage, "--when must be a date or date and time (yyyy-mm-ddThh:mm:ss)");

    Measure m;      // a new reading is a manual entry, as the GUI's Add
    m.when = when;
    m.comment = request.args.value("comment").toString();
    CommandResult values = measureValues(g, request.args.value("set"), m);
    if (!values.ok()) return values;

    // replace a reading at the same time; as in the GUI it keeps its source
    QList<Measure> list = g->measures();
    for (int i = list.count() - 1; i >= 0; i--) {
        if (list.at(i).when != when) continue;
        m.source = list.at(i).source;
        m.originalSource = list.at(i).originalSource;
        list.removeAt(i);
    }
    list.append(m);
    CommandResult saved = saveMeasures(s, g, list);
    if (!saved.ok()) return saved;

    QJsonObject data;
    data.insert("group", g->getSymbol());
    data.insert("when", when.toString(Qt::ISODate));
    data.insert("refreshed", s.rideCache()->lastStaleCount());
    return CommandResult::success(data);
}

static CommandResult
editMeasure(CommandEnvironment &env, const CommandRequest &request)
{
    AthleteSession &s = *env.session;
    MeasuresGroup *g = measuresGroup(s.athlete(), request.args.value("group").toString());
    if (!g) return CommandResult::failure(Status::NotFound, QString("no measures group '%1'").arg(request.args.value("group").toString()));
    if (!request.args.contains("set") && !request.args.contains("comment"))
        return CommandResult::failure(Status::Usage, "give what to change: --set FIELD=VALUE or --comment");

    QList<Measure> list = g->measures();
    CommandResult failure;
    int index = findMeasure(list, request.args.value("when").toString(), failure);
    if (index < 0) return failure;

    // the values given change, the others stay; edited by hand, it becomes a
    // manual entry, as when a value is changed in the GUI's measures table
    Measure m = list.at(index);
    CommandResult values = measureValues(g, request.args.value("set"), m);
    if (!values.ok()) return values;
    if (request.args.contains("comment")) m.comment = request.args.value("comment").toString();
    m.source = Measure::Manual;
    list[index] = m;
    CommandResult saved = saveMeasures(s, g, list);
    if (!saved.ok()) return saved;

    QJsonObject data;
    data.insert("group", g->getSymbol());
    data.insert("when", m.when.toString(Qt::ISODate));
    data.insert("status", "updated");
    data.insert("refreshed", s.rideCache()->lastStaleCount());
    CommandResult result = CommandResult::success(data);
    result.text = QString("updated the %1 reading at %2, %3 activities recomputed\n")
                  .arg(g->getSymbol()).arg(m.when.toString(Qt::ISODate)).arg(s.rideCache()->lastStaleCount());
    return result;
}

static CommandResult
removeMeasure(CommandEnvironment &env, const CommandRequest &request)
{
    AthleteSession &s = *env.session;
    MeasuresGroup *g = measuresGroup(s.athlete(), request.args.value("group").toString());
    if (!g) return CommandResult::failure(Status::NotFound, QString("no measures group '%1'").arg(request.args.value("group").toString()));

    QList<Measure> list = g->measures();
    CommandResult failure;
    int index = findMeasure(list, request.args.value("when").toString(), failure);
    if (index < 0) return failure;
    QDateTime when = list.at(index).when;
    list.removeAt(index);
    CommandResult saved = saveMeasures(s, g, list);
    if (!saved.ok()) return saved;

    QJsonObject data;
    data.insert("group", g->getSymbol());
    data.insert("when", when.toString(Qt::ISODate));
    data.insert("status", "removed");
    data.insert("refreshed", s.rideCache()->lastStaleCount());
    CommandResult result = CommandResult::success(data);
    result.text = QString("removed the %1 reading at %2, %3 activities recomputed\n")
                  .arg(g->getSymbol()).arg(when.toString(Qt::ISODate)).arg(s.rideCache()->lastStaleCount());
    return result;
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

    Command set;
    set.spec.name = "athlete.set";
    set.spec.summary = "change the athlete's details and model settings, as the About and Model tabs";
    set.spec.description =
        "Only the settings you pass change. Weight and height (as the GUI, to one\n"
        "decimal) recompute the metrics that depend on them where no Body measure or\n"
        "activity weight applies; wheel size and crank length recompute as the GUI\n"
        "does. W'bal tau is the tau Train's real time W'bal uses: activity W' metrics\n"
        "work out their own, so nothing is recomputed.";
    set.spec.scope = Scope::Athlete;
    set.spec.modifies = true;
    set.spec.params << ParamSpec("nickname", ParamType::String, "nickname");
    set.spec.params << ParamSpec("dob", ParamType::Date, "date of birth");
    set.spec.params << ParamSpec("sex", ParamType::String, "sex").oneOf({ "male", "female" });
    set.spec.params << ParamSpec("height", ParamType::Double, "height in cm (0 to 999.9)");
    set.spec.params << ParamSpec("weight", ParamType::Double, "default weight in kg (0 to 999.9)");
    set.spec.params << ParamSpec("crank-length", ParamType::Double, "crank length in mm (130 to 220, as the About tab lists them)");
    set.spec.params << ParamSpec("wheel-size", ParamType::Int, "wheel circumference in mm (1 to 9999)");
    set.spec.params << ParamSpec("wbal-tau", ParamType::Int, "W'bal tau in seconds (30 to 1200)");
    set.spec.params << ParamSpec("sts-days", ParamType::Int, "PMC short term stress average in days (1 to 21)");
    set.spec.params << ParamSpec("lts-days", ParamType::Int, "PMC long term stress average in days (7 to 56)");
    set.spec.params << ParamSpec("sb-today", ParamType::String, "PMC stress balance today").oneOf({ "true", "false" });
    set.spec.httpMethod = "PUT";
    set.spec.httpPath = "/athletes/{athlete}";
    set.handler = setAthlete;
    registry.add(set);

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
    zones.spec.params << ParamSpec("imperial", ParamType::Bool, "paces per mile or 100 yards");
    zones.spec.httpMethod = "GET";
    zones.spec.httpPath = "/athletes/{athlete}/zones";
    zones.handler = showZones;
    registry.add(zones);

    Command setz;
    setz.spec.name = "zones.set";
    setz.spec.summary = "add or change a power, heart rate or pace zone range, as the GUI's zones pages";
    setz.spec.description =
        "Sets the values for the range starting on --from (added when there is none).\n"
        "A new range copies anchors you leave out from the range that covered that\n"
        "day, so a new resting heart rate does not reset LTHR or maximum heart rate.\n"
        "Changing an existing range changes only the values you pass. The first range\n"
        "for a sport still needs --cp, --lthr or --cv. Pace is in km/h, for Run or Swim.";
    setz.spec.scope = Scope::Athlete;
    setz.spec.modifies = true;
    setz.spec.params << ParamSpec("type", ParamType::String, "which zones").def("power").oneOf({ "power", "hr", "pace" });
    setz.spec.params << ParamSpec("from", ParamType::Date, "first day the values apply").req();
    setz.spec.params << ParamSpec("sport", ParamType::String, "sport").def("Bike");
    setz.spec.params << ParamSpec("cp", ParamType::Int, "critical power (power)");
    setz.spec.params << ParamSpec("ftp", ParamType::Int, "FTP (power)");
    setz.spec.params << ParamSpec("w", ParamType::Int, "W' in joules (power)");
    setz.spec.params << ParamSpec("pmax", ParamType::Int, "maximal power (power)");
    setz.spec.params << ParamSpec("aet", ParamType::Int, "aerobic threshold (power, hr or pace)");
    setz.spec.params << ParamSpec("lthr", ParamType::Int, "lactate threshold heart rate (hr)");
    setz.spec.params << ParamSpec("resthr", ParamType::Int, "resting heart rate (hr)");
    setz.spec.params << ParamSpec("maxhr", ParamType::Int, "maximum heart rate (hr)");
    setz.spec.params << ParamSpec("cv", ParamType::Double, "critical velocity in km/h (pace)");
    setz.spec.httpMethod = "PUT";
    setz.spec.httpPath = "/athletes/{athlete}/zones";
    setz.handler = setZones;
    registry.add(setz);

    Command removez;
    removez.spec.name = "zones.remove";
    removez.spec.summary = "remove the zone range starting on a day, as the zones pages' delete button";
    removez.spec.description =
        "The range before it is extended to cover its days. The only range of a\n"
        "sport can't be removed: change it with zones set.";
    removez.spec.scope = Scope::Athlete;
    removez.spec.modifies = true;
    removez.spec.params << ParamSpec("type", ParamType::String, "which zones").def("power").oneOf({ "power", "hr", "pace" });
    removez.spec.params << ParamSpec("from", ParamType::Date, "the day the range starts").req();
    removez.spec.params << ParamSpec("sport", ParamType::String, "sport").def("Bike");
    removez.spec.httpMethod = "DELETE";
    removez.spec.httpPath = "/athletes/{athlete}/zones";
    removez.handler = removeZones;
    registry.add(removez);

    Command options;
    options.spec.name = "zones.options";
    options.spec.summary = "set the power zones page's CP model and whether Coggan metrics use CP or FTP";
    options.spec.description =
        "--cp-model is the page's first choice: Manual, or Semi-Automatic from the\n"
        "CP2, CP3 or Extended model (the GUI then offers new ranges from estimates).\n"
        "--coggan-metrics is the second: 'Use CP for all metrics' (cp) or 'Use FTP\n"
        "for Coggan metrics' (ftp), which recomputes the activities. Per sport;\n"
        "zones show prints them.";
    options.spec.scope = Scope::Athlete;
    options.spec.modifies = true;
    options.spec.params << ParamSpec("sport", ParamType::String, "sport").def("Bike");
    options.spec.params << ParamSpec("cp-model", ParamType::String, "how ranges are set").oneOf(cpModels);
    options.spec.params << ParamSpec("coggan-metrics", ParamType::String, "what NP, IF, TSS and the other Coggan metrics use").oneOf(cogganMetrics);
    options.spec.httpMethod = "PUT";
    options.spec.httpPath = "/athletes/{athlete}/zones/options";
    options.handler = setZoneOptions;
    registry.add(options);

    Command schemeShow;
    schemeShow.spec.name = "zones.scheme.show";
    schemeShow.spec.summary = "show the default zones (the zones pages' Default tab) in % of CP, LT or CV";
    schemeShow.spec.scope = Scope::Athlete;
    schemeShow.spec.params << ParamSpec("type", ParamType::String, "which zones").def("power").oneOf({ "power", "hr", "pace" });
    schemeShow.spec.params << ParamSpec("sport", ParamType::String, "sport").def("Bike");
    schemeShow.spec.httpMethod = "GET";
    schemeShow.spec.httpPath = "/athletes/{athlete}/zones/scheme";
    schemeShow.handler = showScheme;
    registry.add(schemeShow);

    Command schemeSet;
    schemeSet.spec.name = "zones.scheme.set";
    schemeSet.spec.summary = "replace the default zones (the zones pages' Default tab)";
    schemeSet.spec.description =
        "Each --zone is NAME,DESCRIPTION,PERCENT, the zone's lower bound in % of CP,\n"
        "LT or CV, with ,TRIMPK added for heart rate. The zones are sorted by their\n"
        "lower bound. Every range whose zones come from the default follows the new\n"
        "zones, including existing ones; a range with zones of its own keeps them.";
    schemeSet.spec.scope = Scope::Athlete;
    schemeSet.spec.modifies = true;
    schemeSet.spec.params << ParamSpec("type", ParamType::String, "which zones").def("power").oneOf({ "power", "hr", "pace" });
    schemeSet.spec.params << ParamSpec("sport", ParamType::String, "sport").def("Bike");
    schemeSet.spec.params << ParamSpec("zone", ParamType::String, "NAME,DESCRIPTION,PERCENT[,TRIMPK], e.g. 'Z2,Endurance,55'").req().many();
    schemeSet.spec.httpMethod = "PUT";
    schemeSet.spec.httpPath = "/athletes/{athlete}/zones/scheme";
    schemeSet.handler = setScheme;
    registry.add(schemeSet);

    Command mlist;
    mlist.spec.name = "measures.list";
    mlist.spec.summary = "list body, HRV and other daily measures";
    mlist.spec.scope = Scope::Athlete;
    mlist.spec.params << ParamSpec("group", ParamType::String, "measures group, e.g. Body or Hrv");
    mlist.spec.params << ParamSpec("from", ParamType::Date, "first day");
    mlist.spec.params << ParamSpec("to", ParamType::Date, "last day");
    mlist.spec.params << seasonParam();
    mlist.spec.httpMethod = "GET";
    mlist.spec.httpPath = "/athletes/{athlete}/measures";
    mlist.handler = listMeasures;
    registry.add(mlist);

    Command madd;
    madd.spec.name = "measures.add";
    madd.spec.summary = "record a measure, e.g. body weight, and recompute what depends on it";
    madd.spec.scope = Scope::Athlete;
    madd.spec.modifies = true;
    madd.spec.params << ParamSpec("group", ParamType::String, "measures group").def("Body");
    madd.spec.params << ParamSpec("when", ParamType::String, "date or date and time").req();
    madd.spec.params << ParamSpec("set", ParamType::String, "FIELD=VALUE, e.g. WEIGHTKG=71.5").req().many();
    madd.spec.params << ParamSpec("comment", ParamType::String, "comment");
    madd.spec.httpMethod = "POST";
    madd.spec.httpPath = "/athletes/{athlete}/measures";
    madd.handler = addMeasure;
    registry.add(madd);

    Command medit;
    medit.spec.name = "measures.edit";
    medit.spec.summary = "change values or the comment of a recorded measure, as editing it in the measures table";
    medit.spec.description =
        "--when is the reading's time as measures list prints it, or a date when\n"
        "only one reading is on it. The values you pass change and the others stay;\n"
        "the reading becomes a manual entry, as in the GUI.";
    medit.spec.scope = Scope::Athlete;
    medit.spec.modifies = true;
    medit.spec.params << ParamSpec("group", ParamType::String, "measures group").def("Body");
    medit.spec.params << ParamSpec("when", ParamType::String, "the reading's date and time, or its date").req();
    medit.spec.params << ParamSpec("set", ParamType::String, "FIELD=VALUE, e.g. WEIGHTKG=71.5").many();
    medit.spec.params << ParamSpec("comment", ParamType::String, "new comment");
    medit.spec.httpMethod = "PUT";
    medit.spec.httpPath = "/athletes/{athlete}/measures";
    medit.handler = editMeasure;
    registry.add(medit);

    Command mremove;
    mremove.spec.name = "measures.remove";
    mremove.spec.summary = "delete a recorded measure and recompute what depends on it";
    mremove.spec.description =
        "--when is the reading's time as measures list prints it, or a date when\n"
        "only one reading is on it.";
    mremove.spec.scope = Scope::Athlete;
    mremove.spec.modifies = true;
    mremove.spec.params << ParamSpec("group", ParamType::String, "measures group").def("Body");
    mremove.spec.params << ParamSpec("when", ParamType::String, "the reading's date and time, or its date").req();
    mremove.spec.httpMethod = "DELETE";
    mremove.spec.httpPath = "/athletes/{athlete}/measures";
    mremove.handler = removeMeasure;
    registry.add(mremove);
}

} // namespace Headless
