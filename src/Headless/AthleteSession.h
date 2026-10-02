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

#ifndef _GC_AthleteSession_h
#define _GC_AthleteSession_h 1

#include "HeadlessCommand.h"
#include "AthleteLock.h"

#include <QString>
#include <QHash>
#include <QStringList>
#include <memory>

class Context;
class Athlete;
class RideCache;
class RideItem;


namespace Headless {

//
// One athlete opened without a main window, for the duration of a command.
//
// Opening takes the athlete lock, loads the ride cache and brings it up to
// date with the activity and zone files on disk (the same refresh the GUI
// performs when an athlete is opened), so every command sees the files as
// they are at the moment it starts. Destroying the session saves the cache,
// marks the athlete as cleanly closed and releases the lock.
//
class AthleteSession
{
    public:

        struct Options {
            int lockWaitSeconds = 0;    // wait for another process to finish
            bool force = false;         // open even if not closed cleanly
        };

        // returns nullptr and sets failure (status and message) when the
        // athlete can't be opened
        static std::unique_ptr<AthleteSession> open(const QString &home, const QString &name,
                                                    const Options &options, CommandResult &failure);

        ~AthleteSession();

        Context *context() const { return context_; }
        Athlete *athlete() const { return athlete_; }
        RideCache *rideCache() const;
        QString name() const { return name_; }
        QString folder() const { return folder_; }

        // wait for any running metric refresh to finish
        void waitForRefresh();

        // check the files on disk again and recompute what changed
        void refresh();

        // number of activities that were recomputed when the session opened
        int refreshedOnOpen() const { return refreshedOnOpen_; }

        // wait for the model estimates (CP, W' ...) to be computed
        void waitForEstimates();

        // find an activity by file name, base name, start date-time or index;
        // planned ones with planned
        RideItem *findActivity(const QString &id, QString &error, bool planned = false) const;

    private:

        AthleteSession() {}

        QString home_, name_, folder_;
        Context *context_ = nullptr;
        Athlete *athlete_ = nullptr;
        std::unique_ptr<AthleteLock> lock_;
        int refreshedOnOpen_ = 0;
};

// finds activities by id as findActivity does, with the rides indexed
// once, for looking up many. Valid while the rides don't change.
// With planned, dates, start times, 'first' and 'last' are planned
// activities, and a planned activity wins over a completed one of the same
// file name (the two are kept in different folders).
class ActivityLookup
{
    public:
        explicit ActivityLookup(RideCache *cache, bool planned = false);
        RideItem *find(const QString &id, QString &error) const;

    private:
        bool open = false;
        bool planned = false;
        QList<RideItem *> actual;               // of the kind looked for, in date order
        QHash<QString, RideItem *> byFile;      // file name, and without its suffix
};

// the ride cache adds, replaces and deletes activities by file name, planned
// or completed alike, so a planned and a completed activity of the same file
// name get confused: the activity of the other kind called fileName, if any
RideItem *otherKindNamed(RideCache *cache, const QString &fileName, bool planned);

// refusing to make such a pair: a planned (or completed) activity would be
// called fileName, which the other kind already is. Empty when it isn't
QString sameNameRefusal(RideCache *cache, const QString &fileName, bool planned);

// a plain folder name, no path separators and not hidden
bool isAthleteName(const QString &name);

// settings shared by all athletes (metadata fields, python processors) live
// in the athletes folder; returns a description of who is using an athlete
// there, or an empty string when none is open elsewhere
QString athletesInUse(const QString &home);

class CommandEnvironment;

// commands on the athletes folder: it must exist and be set up
CommandResult requireHome(const CommandEnvironment &env);

// ...and settings shared by all athletes may only change when no other
// GoldenCheetah is using an athlete in the folder (it would overwrite them)
CommandResult sharedSettingsWritable(const CommandEnvironment &env);

// what a command handler gets to work with
class CommandEnvironment
{
    public:

        QString home;                       // athletes root folder
        AthleteSession *session = nullptr;  // set for athlete commands

        // report progress, e.g. to stderr on the command line
        std::function<void(const QString &)> progress;

        void report(const QString &message) const { if (progress) progress(message); }
};

} // namespace Headless

#endif
