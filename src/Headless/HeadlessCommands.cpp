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

#include "GcUpgrade.h"
#include "RideMetric.h"
#include "RideFile.h"

#include <QDir>
#include <QSysInfo>
#include <QFileInfo>
#include <exception>

namespace Headless {

static CommandRegistry
buildRegistry()
{
    CommandRegistry registry;
    registerSystemCommands(registry);
    registerAthleteCommands(registry);
    registerActivityCommands(registry);
    registerIntervalCommands(registry);
    registerOverviewCommands(registry);
    registerImportCommands(registry);
    registerFieldCommands(registry);
    registerProcessorCommands(registry);
    registerMetricCommands(registry);
    registerUserMetricCommands(registry);
    registerNavigatorCommands(registry);
    registerChartCommands(registry);
    registerChartLibraryCommands(registry);
    registerSeasonCommands(registry);
    registerPlanCommands(registry);
    return registry;
}

const CommandRegistry &
commandRegistry()
{
    // built once, thread-safe
    static const CommandRegistry registry = buildRegistry();
    return registry;
}

QString
CommandRunner::resolveAthlete(const QString &home, const QString &given, QString &error)
{
    error.clear();
    if (!given.isEmpty()) return given;

    QStringList names = HeadlessApp::athletes(home);
    if (names.count() == 1) return names.first();
    if (names.isEmpty()) error = QString("there are no athletes in %1").arg(home);
    else error = QString("more than one athlete in %1, choose one with --athlete (%2)").arg(home).arg(names.join(", "));
    return QString();
}

CommandResult
CommandRunner::run(const CommandRegistry &registry, CommandRequest request, const Options &options)
{
    const Command *command = registry.find(request.command);
    if (!command) return CommandResult::failure(Status::Usage, QString("unknown command '%1'").arg(request.command));

    QString error = CommandRegistry::validate(command->spec, request.args);
    if (!error.isEmpty()) return CommandResult::failure(Status::Usage, error);

    CommandEnvironment env;
    env.home = request.home;
    env.progress = options.progress;

    try {
        if (command->spec.scope == Scope::Global) {
            return command->handler(env, request);
        }

        // athlete commands need an initialised core and an athlete
        if (!HeadlessApp::isInitialised())
            return CommandResult::failure(Status::NotFound, HeadlessApp::missingHome(request.home));

        QString athlete = resolveAthlete(request.home, request.athlete, error);
        if (athlete.isEmpty()) return CommandResult::failure(Status::NotFound, error);
        request.athlete = athlete;

        CommandResult failure;
        std::unique_ptr<AthleteSession> session = AthleteSession::open(request.home, athlete, options.session, failure);
        if (!session) return failure;

        env.session = session.get();
        CommandResult result = command->handler(env, request);

        // closed (and saved) when the session goes out of scope, before we
        // report success, so a caller never sees the athlete still open
        env.session = nullptr;
        session.reset();
        return result;

    } catch (const std::exception &e) {
        return CommandResult::failure(Status::Internal, QString("internal error: %1").arg(e.what()));
    } catch (...) {
        return CommandResult::failure(Status::Internal, "internal error");
    }
}

//
// System commands
//

static CommandResult
versionCommand(CommandEnvironment &, const CommandRequest &)
{
    QJsonObject data;
    data.insert("version", QString(VERSION_STRING));
    data.insert("build", VERSION_LATEST);
    data.insert("schema", DBSchemaVersion);
    data.insert("metrics", RideMetricFactory::instance().metricCount());
    data.insert("qt", QString(qVersion()));
    data.insert("python", HeadlessApp::pythonAvailable());
    data.insert("host", QSysInfo::machineHostName()); // as recorded in athlete locks
    data.insert("import_formats", RideFileFactory::instance().suffixes().count());
    return CommandResult::success(data);
}

static CommandResult
commandsCommand(CommandEnvironment &, const CommandRequest &)
{
    QJsonArray list;
    for (const Command *c : commandRegistry().commands()) list.append(CommandRegistry::describe(c->spec));
    QJsonObject data;
    data.insert("commands", list);
    return CommandResult::success(data);
}

void
registerSystemCommands(CommandRegistry &registry)
{
    Command version;
    version.spec.name = "version";
    version.spec.summary = "show version and build information";
    version.spec.scope = Scope::Global;
    version.spec.httpMethod = "GET";
    version.spec.httpPath = "/version";
    version.handler = versionCommand;
    registry.add(version);

    Command commands;
    commands.spec.name = "commands";
    commands.spec.summary = "describe every command and its parameters (machine readable)";
    commands.spec.scope = Scope::Global;
    commands.spec.httpMethod = "GET";
    commands.spec.httpPath = "/commands";
    commands.handler = commandsCommand;
    registry.add(commands);
}

} // namespace Headless
