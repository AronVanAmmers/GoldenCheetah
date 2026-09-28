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

#ifndef _GC_RestServer_h
#define _GC_RestServer_h 1

#include "AthleteSession.h"

#include <QString>

namespace Headless {

//
// The REST entry point: every command over HTTP under /v1, JSON in and out.
//
// Requests are run one at a time on the main thread through the same
// CommandRunner the command line uses, so each request opens, refreshes
// and closes the athlete exactly as a command line run does, and the GUI
// can open the athlete between requests.
//
class RestServer
{
    public:

        struct Options {
            QString host = "127.0.0.1";
            int port = 12022;
            QString token;                  // required bearer token, empty = none
            qint64 maxUploadBytes = 64LL * 1024 * 1024;
            QString home;                   // athletes folder
            AthleteSession::Options session;
            bool quiet = false;             // no request log on stderr
        };

        // serve until SIGINT/SIGTERM, returns the exit status
        static int run(const Options &options);
};

} // namespace Headless

#endif
