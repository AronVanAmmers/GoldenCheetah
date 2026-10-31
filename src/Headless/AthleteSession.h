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
#include <QSet>
#include <QStringList>
#include <QElapsedTimer>
#include <memory>
#include <vector>

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

        // after a command saved activities: recompute those it saved or
        // added, and any marked stale, as refresh() would, without checking
        // the files of all the others (nothing else can have changed them
        // while the athlete is locked; on a slow folder that check is most
        // of a save's time). Changes every activity depends on (zones,
        // measures, settings) need refresh().
        void refreshChanged();

        // after a change of settings every activity may depend on (weight,
        // zones): each is checked against it, as refresh() does, but not
        // against its files, which the athlete's lock keeps as they were
        void refreshSettings();

        // number of activities that were recomputed when the session opened
        int refreshedOnOpen() const { return refreshedOnOpen_; }

        // every command starts with this. A command that only reads the
        // athlete leaves the ride cache on disk alone when the session closes
        // (tens of megabytes on a large library) unless a refresh recomputed
        // activities, which is worth keeping. In a session kept open for
        // several commands (SessionKeeper) the cache is saved when any of
        // them changed something, and only the first one reports the
        // refresh made when the athlete was opened.
        void commandStarts(bool modifies);

        // ...and a command in a kept session ends with this: what closing
        // the athlete would do that the next command must not miss
        void commandEnds();

        // the athlete was opened for the running command, not kept open
        // from an earlier one (whose refresh the files may have outlived)
        bool openedForThisCommand() const { return commands_ <= 1; }

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
        int commands_ = 0;          // started in this session
        bool modifies_ = false;     // the running one changes the athlete
        QSet<QString> known_;       // activities (path) when the command started
        QSet<RideItem *> saved_;    // saved since
        bool readOnly_ = false;
        bool recomputed_ = false;   // a refresh changed what the cache holds
};

//
// Athletes kept open from one command to the next, for the session server
// (session start) and serve --keep-open: opening a large athlete reads the
// ride cache and checks every activity file, which takes long on a slow
// folder (a network share, a Windows drive seen from WSL).
//
// A kept athlete stays locked, so no other GoldenCheetah changes it in
// between. Tools that edit its files directly are not seen until athlete
// refresh (activity files) or the athlete is opened again (anything else).
//
class SessionKeeper
{
    public:

        ~SessionKeeper() { closeAll(); }

        // the athlete, opened now when it isn't open already; nullptr with
        // failure set when it can't be opened
        AthleteSession *open(const QString &home, const QString &name,
                             const AthleteSession::Options &options, CommandResult &failure);

        // after each command. One that failed unexpectedly may have left the
        // athlete in any state: it is closed, the next command opens it afresh
        void done(AthleteSession *session, const CommandResult &result);

        // closed and saved, as at the end of a single command
        void close(AthleteSession *session);
        void closeAll();

        // those no command has used for idleMs, returns how many
        int closeIdle(qint64 idleMs);

        AthleteSession *find(const QString &home, const QString &name) const;
        int count() const { return int(kept.size()); }

    private:

        struct Kept {
            std::unique_ptr<AthleteSession> session;
            QElapsedTimer used;
        };
        std::vector<Kept> kept;
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
