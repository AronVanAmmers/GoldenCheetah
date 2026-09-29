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

#ifndef _GC_CliParser_h
#define _GC_CliParser_h 1

#include "CommandRegistry.h"

namespace Headless {

// options that apply to every command
struct GlobalOptions {
    QString home;           // --home DIR          athletes root folder
    QString athlete;        // --athlete NAME      athlete folder in home
    QString athleteDir;     // --athlete-dir DIR   full path, sets home and athlete
    QString format = "text";// --format text|json|csv
    QString output;         // --output FILE       where binary output goes
    bool quiet = false;     // --quiet             only errors
    bool verbose = false;   // --verbose           GoldenCheetah diagnostics to stderr
    bool noPython = false;  // --no-python
    bool force = false;     // --force             open an athlete that did not close cleanly
    int lockWait = 0;       // --lock-wait SECS    wait for the athlete lock
};

struct CliParse {
    GlobalOptions global;
    QString command;        // dotted name, empty when only help was asked for
    QJsonObject args;       // raw (string) values, validated later by the registry
    bool help = false;      // --help, or "help [topic]"
    QString helpTopic;      // command or group the help is about
    bool version = false;
    QString error;          // parse error, usage should be shown
};

class CliParser
{
    public:

        // argv without the program name and the --cli switch
        static CliParse parse(const QStringList &argv, const CommandRegistry &registry);

        // help texts
        static QString usage(const CommandRegistry &registry, const QString &program);
        static QString commandHelp(const CommandSpec &spec, const QString &program);
        static QString groupHelp(const CommandRegistry &registry, const QString &group, const QString &program);

        // "dry-run" <-> "--dry-run"
        static QString optionName(const ParamSpec &p) { return "--" + p.name; }
};

} // namespace Headless

#endif
