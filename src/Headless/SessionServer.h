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

#ifndef _GC_SessionServer_h
#define _GC_SessionServer_h 1

#include "AthleteSession.h"

#include <QString>
#include <QStringList>
#include <functional>

namespace Headless {

//
// An athlete kept open between command line runs: session start opens it in
// a background process, the commands that follow run in that process, and
// session stop closes it.
//
// Every command otherwise opens the athlete, which reads the ride cache and
// checks each activity file: seconds on a local disk, much longer on a slow
// folder (a Windows drive seen from WSL, a network share). In a session only
// the first command pays for that.
//
// The command line finds the session itself: while one is open for the
// athlete, a command on it is sent to the session and its result written out
// as if it had run in the command's own process. So is a command changing
// settings the athletes folder keeps for every athlete (field add, processor
// install): those are refused while an athlete is open elsewhere, and the
// session's athlete uses them. The session listens on a local socket next to
// the athlete lock (a named pipe on Windows) that only this user can open,
// and runs one command at a time.
//
class SessionServer
{
    public:

        struct Options {
            QString home;               // athletes folder
            QString athlete;            // athlete folder name in home
            AthleteSession::Options session;
            double idleMinutes = 30;    // stop when no command came for this long, 0 = never
            bool quiet = false;         // nothing on stderr when it opens and stops
        };

        // open the athlete and run commands until stopped (session stop,
        // SIGINT, SIGTERM or idle), in this process; returns the exit status
        static int run(const Options &options);

        // run() in a background process, started as program with arguments,
        // waiting until it has the athlete open: the session, or why not
        static CommandResult start(const Options &options, const QString &program, const QStringList &arguments);
};

class SessionClient
{
    public:

        // the server of the session open for athlete in home, empty when
        // there is none. With no athlete: the only session open in home.
        static QString find(const QString &home, const QString &athlete);

        // run the request in the session, false when it can't be reached
        // (the command didn't run). progress gets the command's progress.
        static bool run(const QString &server, const CommandRequest &request,
                        const std::function<void(const QString &)> &progress, CommandResult &result);

        // what the session is doing, and closing it (saved and unlocked
        // when stop returns)
        static CommandResult status(const QString &server);
        static CommandResult stop(const QString &server);
};

} // namespace Headless

#endif
