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

#ifndef _GC_HeadlessCommands_h
#define _GC_HeadlessCommands_h 1

#include "CommandRegistry.h"
#include "AthleteSession.h"

namespace Headless {

// each domain registers its commands
void registerSystemCommands(CommandRegistry &registry);
void registerAthleteCommands(CommandRegistry &registry);
void registerActivityCommands(CommandRegistry &registry);
void registerIntervalCommands(CommandRegistry &registry);
void registerOverviewCommands(CommandRegistry &registry);
void registerImportCommands(CommandRegistry &registry);
void registerFieldCommands(CommandRegistry &registry);
void registerProcessorCommands(CommandRegistry &registry);
void registerMetricCommands(CommandRegistry &registry);
void registerChartCommands(CommandRegistry &registry);

// the full command table used by the command line and REST server
const CommandRegistry &commandRegistry();

//
// Runs one command request: validates the arguments, opens (and afterwards
// closes) the athlete for athlete commands and calls the handler. This is
// the single place the command line and REST server enter the core.
//
class CommandRunner
{
    public:

        struct Options {
            AthleteSession::Options session;
            std::function<void(const QString &)> progress;
        };

        static CommandResult run(const CommandRegistry &registry, CommandRequest request, const Options &options);

        // the athlete a request is for: as given, or the only athlete in home
        static QString resolveAthlete(const QString &home, const QString &given, QString &error);
};

} // namespace Headless

#endif
