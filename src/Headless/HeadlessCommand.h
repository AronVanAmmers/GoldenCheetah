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

#ifndef _GC_HeadlessCommand_h
#define _GC_HeadlessCommand_h 1

//
// The headless command model shared by every non-GUI entry point.
//
// A command is described once by a CommandSpec (its name, parameters and
// REST route) and implemented once by a handler. The command line and the
// REST server are thin adapters: they turn argv or an HTTP request into a
// CommandRequest, run it through the CommandRegistry and render the
// CommandResult as text, JSON or an HTTP response.
//
// Nothing in this header depends on GoldenCheetah internals, so it can be
// unit tested without an athlete.
//

#include <QString>
#include <QStringList>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QByteArray>
#include <QMap>
#include <functional>

namespace Headless {

enum class ParamType {
    String,     // free text
    Int,        // integer
    Double,     // floating point
    Bool,       // true/false, a flag on the command line
    Date,       // yyyy-MM-dd
    Path        // local file or directory, resolved against the working dir
};

QString paramTypeName(ParamType type);

struct ParamSpec {
    QString name;               // canonical name, e.g. "filter"
    ParamType type = ParamType::String;
    QString description;
    bool required = false;
    bool positional = false;    // on the command line may be given without --name
    bool repeated = false;      // may be given many times, value is an array
    QJsonValue defaultValue;    // used when not supplied
    QStringList choices;        // allowed values, empty means any
    bool commandLine = false;   // reads a file where the command runs: refused over REST
    bool uploads = false;       // over REST, uploaded files arrive here (as their paths)

    ParamSpec() {}
    ParamSpec(const QString &name, ParamType type, const QString &description)
        : name(name), type(type), description(description) {}

    // fluent helpers so command tables read well
    ParamSpec &req() { required = true; return *this; }
    ParamSpec &pos() { positional = true; return *this; }
    ParamSpec &many() { repeated = true; return *this; }
    ParamSpec &def(const QJsonValue &v) { defaultValue = v; return *this; }
    ParamSpec &oneOf(const QStringList &c) { choices = c; return *this; }
    ParamSpec &cliOnly() { commandLine = true; return *this; }
    ParamSpec &upload() { uploads = true; return *this; }
};

// does a command need an athlete opened before it runs?
enum class Scope {
    Global,     // works on the athletes root folder (or nothing at all)
    Athlete     // needs one athlete opened (and locked)
};

struct CommandSpec {
    QString name;               // dotted, e.g. "activity.list"
    QString summary;            // one line
    QString description;        // longer help, may be empty
    Scope scope = Scope::Athlete;
    bool modifies = false;      // writes to the athlete or root folder
    QList<ParamSpec> params;

    // REST binding, e.g. "GET" "/athletes/{athlete}/activities/{activity}"
    // path parameters bind to params of the same name ({athlete} is special)
    QString httpMethod;
    QString httpPath;

    const ParamSpec *param(const QString &name) const;

    // "activity.list" -> "activity list"
    QString cliName() const { return QString(name).replace('.', ' '); }
};

// exit status, also mapped to HTTP status codes by the REST server
enum class Status {
    Ok = 0,
    Partial = 1,        // ran, but some items failed (see data)
    Usage = 2,          // bad arguments
    NotFound = 3,       // athlete, activity, processor ... doesn't exist
    Locked = 4,         // athlete is in use by another process
    Failed = 5,         // could not do what was asked
    Internal = 6        // unexpected failure
};

int httpStatusFor(Status status);
QString statusName(Status status);

struct CommandRequest {
    QString command;            // dotted name
    QJsonObject args;           // parameter values, keyed by ParamSpec::name

    // where to find the athlete, resolved by the entry point
    QString home;               // athletes root folder
    QString athlete;            // athlete folder name within home

    // uploaded files are temporary copies: path -> the name the client sent,
    // for reports
    QMap<QString, QString> displayNames;
};

struct CommandResult {
    Status status = Status::Ok;
    QString error;              // set when status is not Ok
    QJsonObject data;           // structured result
    QStringList warnings;

    // binary or file output (a chart image, an exported activity) is carried
    // here so the REST server can stream it; the command line writes it to
    // the --output file or stdout
    QByteArray payload;
    QString payloadType;        // MIME type
    QString payloadName;        // suggested file name

    // optional human readable rendering, used by the command line instead of
    // the generic formatter when present
    QString text;

    // optional CSV rendering, for results the generic one can't lay out
    // as a single table (see ResultFormat::csv)
    QString csv;

    bool ok() const { return status == Status::Ok; }

    static CommandResult success(const QJsonObject &data = QJsonObject());
    static CommandResult failure(Status status, const QString &error);
};

// the part of the core a handler needs, implemented by the session layer
// so the command model itself stays free of GoldenCheetah dependencies
class CommandEnvironment;

typedef std::function<CommandResult(CommandEnvironment &, const CommandRequest &)> CommandHandler;

struct Command {
    CommandSpec spec;
    CommandHandler handler;
};

} // namespace Headless

#endif
