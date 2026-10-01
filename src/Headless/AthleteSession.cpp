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

#include "AthleteSession.h"
#include "AthleteLock.h"

#include "Context.h"
#include "Athlete.h"
#include "RideCache.h"
#include "RideItem.h"
#include "Estimator.h"
#include "PMCData.h"
#include "Banister.h"
#include "CalendarSync.h"
#include "CloudService.h"
#include "Settings.h"
#include "HeadlessApp.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QThread>

namespace Headless {

bool
isAthleteName(const QString &name)
{
    if (name.isEmpty() || name.startsWith(".")) return false;
    if (name.contains('/') || name.contains('\\') || name.contains(':')) return false;
    return QFileInfo(name).fileName() == name;
}

std::unique_ptr<AthleteSession>
AthleteSession::open(const QString &home, const QString &name, const Options &options, CommandResult &failure)
{
    // a plain folder name inside home, never a path: opening a folder as an
    // athlete creates subfolders and runs the upgrade steps in it
    if (!isAthleteName(name)) {
        failure = CommandResult::failure(Status::Usage, QString("'%1' is not an athlete name").arg(name));
        return nullptr;
    }

    QDir root(home);
    QString folder = root.absoluteFilePath(name);
    QFileInfo info(folder);

    if (!info.exists() || !info.isDir()) {
        failure = CommandResult::failure(Status::NotFound, QString("athlete '%1' not found in %2").arg(name).arg(home));
        return nullptr;
    }
    if (!HeadlessApp::athletes(home).contains(name)) {
        failure = CommandResult::failure(Status::NotFound,
                    QString("%1 is not an athlete folder (it has no config or activities folder)").arg(info.absoluteFilePath()));
        return nullptr;
    }
    folder = info.canonicalFilePath();

    std::unique_ptr<AthleteSession> session(new AthleteSession());
    session->home_ = QFileInfo(home).canonicalFilePath();
    session->name_ = name;
    session->folder_ = folder;

    // one process at a time: the GUI and other command line runs hold this
    session->lock_.reset(new AthleteLock(folder));
    if (!session->lock_->tryLock(options.lockWaitSeconds * 1000)) {
        failure = CommandResult::failure(Status::Locked,
                    QString("athlete '%1' is in use by %2; close it there first").arg(name).arg(session->lock_->holder()));
        return nullptr;
    }

    // settings for this athlete
    appsettings->initializeQSettingsAthlete(session->home_, name);

    // a GoldenCheetah that doesn't take the lock (older versions) marks the
    // athlete as open with the safe exit flag, so does a crash
    if (!options.force && !appsettings->cvalue(name, GC_SAFEEXIT, true).toBool()) {
        failure = CommandResult::failure(Status::Locked,
                    QString("athlete '%1' is open in GoldenCheetah or was not closed cleanly; "
                            "close GoldenCheetah, or use --force if it is not running").arg(name));
        return nullptr;
    }

    // open it, this blocks until the ride cache is loaded and starts the
    // refresh of anything that changed on disk since it was last opened
    session->context_ = new Context(nullptr);
    session->athlete_ = new Athlete(session->context_, QDir(folder));

    // the constructor returns early if the athlete folder upgrade failed
    if (session->athlete_->rideCache == nullptr) {
        failure = CommandResult::failure(Status::Failed, QString("athlete '%1' could not be upgraded to this version").arg(name));
        return nullptr;
    }

    session->waitForRefresh();
    session->refreshedOnOpen_ = session->rideCache()->lastStaleCount();
    return session;
}

AthleteSession::~AthleteSession()
{
    if (athlete_) {

        // don't leave a refresh or estimate running while we tear down
        if (athlete_->rideCache) {
            athlete_->rideCache->cancel();
            athlete_->close();
        }

        // Athlete and RideCache leave these to the process exit: in the GUI
        // views may still point at the activities when the athlete closes.
        // A session has no views, and in the REST server every request
        // opens the athlete again, so free them.
        QVector<RideItem *> items = athlete_->rideCache ? athlete_->rideCache->rides() : QVector<RideItem *>();
        AthleteDirectoryStructure *home = athlete_->home;
        qDeleteAll(athlete_->pmcData);
        athlete_->pmcData.clear();
        qDeleteAll(athlete_->banisterData);
        athlete_->banisterData.clear();
        delete athlete_->calendarSync;
        athlete_->calendarSync = nullptr;
        delete athlete_->cloudAutoDownload;     // never started when headless
        athlete_->cloudAutoDownload = nullptr;

        // the ride cache is written to disk in the destructor
        delete athlete_;
        athlete_ = nullptr;

        // an activity tells the athlete's ride cache it is going, there is none
        // now. An activity doesn't delete its intervals (the cache loader hands
        // them over from a temporary item), these are the ones it owns
        context_->athlete = nullptr;
        for (RideItem *item : items) qDeleteAll(item->intervals());
        qDeleteAll(items);
        delete home;
    }
    delete context_;
    context_ = nullptr;

    // make sure settings are on disk before others may open the athlete
    appsettings->syncQSettings();

    // lock goes last
    lock_.reset();
}

RideCache *
AthleteSession::rideCache() const
{
    return athlete_ ? athlete_->rideCache : nullptr;
}

void
AthleteSession::waitForRefresh()
{
    RideCache *cache = rideCache();
    if (!cache) return;

    // refresh threads report back via queued signals, keep the loop turning
    QCoreApplication::processEvents();
    while (cache->isRunning()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    QCoreApplication::processEvents();
}

void
AthleteSession::refresh()
{
    RideCache *cache = rideCache();
    if (!cache) return;
    cache->refresh();
    waitForRefresh();
}

void
AthleteSession::waitForEstimates()
{
    RideCache *cache = rideCache();
    if (!cache) return;
    Estimator *estimator = cache->getEstimator();
    if (!estimator) return;

    // run now rather than after the lazy delay the GUI uses
    estimator->wait();
    estimator->calculate();
    estimator->wait();
    QCoreApplication::processEvents();
}

RideItem *
AthleteSession::findActivity(const QString &id, QString &error) const
{
    error.clear();
    RideCache *cache = rideCache();
    if (!cache) {
        error = "athlete not open";
        return nullptr;
    }

    QString key = id.trimmed();
    QList<RideItem *> actual;
    for (RideItem *item : cache->rides()) if (!item->planned) actual << item;

    if (key.isEmpty()) {
        error = "no activity given";
        return nullptr;
    }

    // most recent / first
    if (key == "last" || key == "latest") {
        if (actual.isEmpty()) { error = "there are no activities"; return nullptr; }
        return actual.last();
    }
    if (key == "first") {
        if (actual.isEmpty()) { error = "there are no activities"; return nullptr; }
        return actual.first();
    }

    // file name, with or without the suffix, planned included
    for (RideItem *item : cache->rides()) {
        if (item->fileName == key || QFileInfo(item->fileName).completeBaseName() == key) return item;
    }

    // start date and time, in local time as shown by the GUI
    // (a bare date parses as midnight, it is handled below)
    QDateTime when;
    if (key.contains(':')) when = QDateTime::fromString(key, Qt::ISODate);
    if (!when.isValid()) when = QDateTime::fromString(key, "yyyy-MM-dd HH:mm:ss");
    if (!when.isValid()) when = QDateTime::fromString(key, "yyyy-MM-dd HH:mm");
    if (when.isValid()) {
        for (RideItem *item : actual) if (item->dateTime == when) return item;
        error = QString("no activity starts at %1").arg(key);
        return nullptr;
    }

    // a date, if only one activity on that day
    QDate day = QDate::fromString(key, Qt::ISODate);
    if (day.isValid()) {
        QList<RideItem *> matches;
        for (RideItem *item : actual) if (item->dateTime.date() == day) matches << item;
        if (matches.count() == 1) return matches.first();
        if (matches.isEmpty()) error = QString("no activity on %1").arg(key);
        else error = QString("%1 activities on %2, give the start time or file name").arg(matches.count()).arg(key);
        return nullptr;
    }

    error = QString("activity '%1' not found").arg(key);
    return nullptr;
}

QString
athletesInUse(const QString &home)
{
    for (const QString &name : HeadlessApp::athletes(home)) {
        QString folder = QDir(home).absoluteFilePath(name);
        QString holder;
        if (AthleteLock::peek(folder, &holder) == AthleteLock::State::InUse)
            return QString("athlete '%1' is open in %2").arg(name).arg(holder);
    }
    return QString();
}

CommandResult
requireHome(const CommandEnvironment &env)
{
    if (!HeadlessApp::isInitialised())
        return CommandResult::failure(Status::NotFound, HeadlessApp::missingHome(env.home));
    return CommandResult::success();
}

CommandResult
sharedSettingsWritable(const CommandEnvironment &env)
{
    CommandResult home = requireHome(env);
    if (!home.ok()) return home;
    QString who = athletesInUse(env.home);
    if (!who.isEmpty())
        return CommandResult::failure(Status::Locked,
                    QString("%1; settings shared by all athletes can't be changed while GoldenCheetah is using them").arg(who));
    return CommandResult::success();
}

} // namespace Headless
