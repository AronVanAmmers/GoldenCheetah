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

#include "CommandRegistry.h"

#include <cmath>

#include <QDate>
#include <QDir>
#include <QFileInfo>

namespace Headless {

//
// HeadlessCommand.h helpers
//

QString
paramTypeName(ParamType type)
{
    switch (type) {
    case ParamType::String: return "string";
    case ParamType::Int: return "int";
    case ParamType::Double: return "number";
    case ParamType::Bool: return "bool";
    case ParamType::Date: return "date";
    case ParamType::Path: return "path";
    }
    return "string";
}

const ParamSpec *
CommandSpec::param(const QString &name) const
{
    for (const ParamSpec &p : params)
        if (p.name == name) return &p;
    return nullptr;
}

int
httpStatusFor(Status status)
{
    switch (status) {
    case Status::Ok: return 200;
    case Status::Partial: return 207;
    case Status::Usage: return 400;
    case Status::NotFound: return 404;
    case Status::Locked: return 409;
    case Status::Failed: return 422;
    case Status::Internal: return 500;
    }
    return 500;
}

QString
statusName(Status status)
{
    switch (status) {
    case Status::Ok: return "ok";
    case Status::Partial: return "partial";
    case Status::Usage: return "usage";
    case Status::NotFound: return "not_found";
    case Status::Locked: return "locked";
    case Status::Failed: return "failed";
    case Status::Internal: return "internal";
    }
    return "internal";
}

CommandResult
CommandResult::success(const QJsonObject &data)
{
    CommandResult r;
    r.status = Status::Ok;
    r.data = data;
    return r;
}

CommandResult
CommandResult::failure(Status status, const QString &error)
{
    CommandResult r;
    r.status = status;
    r.error = error;
    return r;
}

CommandResult
CommandResult::batch(const QJsonObject &data, int failed, int total, const QString &error)
{
    CommandResult r = success(data);
    if (failed) {
        r.status = failed < total ? Status::Partial : Status::Failed;
        r.error = error;
    }
    return r;
}

//
// Registry
//

void
CommandRegistry::add(const Command &command)
{
    // mistakes in a command's spec, said on every run so they are noticed
    for (const ParamSpec &p : command.spec.params) {
        QString problem;
        // the REST server takes these for itself
        if (p.name == "format" || p.name == "envelope") problem = "the name is reserved";
        // a default must be a value the parameter accepts
        else if (!p.defaultValue.isUndefined() && !p.defaultValue.isNull()) coerce(p, p.defaultValue, problem);
        if (!problem.isEmpty()) {
            qWarning("%s: parameter '%s': %s", qPrintable(command.spec.name), qPrintable(p.name), qPrintable(problem));
            Q_ASSERT_X(false, "CommandRegistry::add", "bad parameter spec");
        }
    }
    table.insert(command.spec.name, command);
}

const Command *
CommandRegistry::find(const QString &name) const
{
    auto it = table.constFind(name);
    if (it == table.constEnd()) return nullptr;
    return &it.value();
}

QList<const Command *>
CommandRegistry::commands() const
{
    QList<const Command *> list;
    for (auto it = table.constBegin(); it != table.constEnd(); ++it) list << &it.value();
    return list;
}

QStringList
CommandRegistry::names() const
{
    return table.keys();
}

QStringList
CommandRegistry::namesWithPrefix(const QString &prefix) const
{
    QStringList list;
    for (const QString &name : table.keys())
        if (name == prefix || name.startsWith(prefix + ".")) list << name;
    return list;
}

static bool
parseBool(const QString &text, bool &ok)
{
    QString t = text.trimmed().toLower();
    ok = true;
    if (t == "1" || t == "true" || t == "yes" || t == "on" || t == "") return true;
    if (t == "0" || t == "false" || t == "no" || t == "off") return false;
    ok = false;
    return false;
}

QJsonValue
CommandRegistry::coerce(const ParamSpec &param, const QJsonValue &value, QString &error)
{
    error.clear();

    // values from argv and URLs arrive as strings, JSON bodies are typed
    const bool isString = value.isString();
    const QString text = value.toString();

    QJsonValue result;
    switch (param.type) {

    case ParamType::String:
        if (value.isString()) result = value;
        else if (value.isDouble()) result = QString::number(value.toDouble(), 'g', 15);
        else if (value.isBool()) result = value.toBool() ? QString("true") : QString("false");
        else error = QString("expected text");
        break;

    case ParamType::Path:
        if (!isString || text.isEmpty()) {
            error = QString("expected a path");
        } else {
            // leave "-" (stdin/stdout) alone, otherwise make absolute so the
            // core doesn't depend on the working directory
            result = (text == "-") ? text : QDir::cleanPath(QFileInfo(text).absoluteFilePath());
        }
        break;

    case ParamType::Int:
        if (value.isDouble()) {
            // in range before the cast, which is undefined otherwise
            double d = value.toDouble();
            if (!std::isfinite(d) || std::fabs(d) >= 9.2e18 || d != std::floor(d)) error = QString("expected a whole number");
            else result = qint64(d);
        } else if (isString) {
            bool ok = false;
            qint64 v = text.trimmed().toLongLong(&ok);
            if (!ok) error = QString("expected a whole number, got '%1'").arg(text);
            else result = v;
        } else error = QString("expected a whole number");
        break;

    case ParamType::Double:
        if (value.isDouble()) result = value;
        else if (isString) {
            bool ok = false;
            double v = text.trimmed().toDouble(&ok);
            if (!ok || !std::isfinite(v)) error = QString("expected a number, got '%1'").arg(text);
            else result = v;
        } else error = QString("expected a number");
        break;

    case ParamType::Bool:
        if (value.isBool()) result = value;
        else if (value.isDouble()) result = value.toDouble() != 0;
        else if (isString) {
            bool ok = false;
            bool v = parseBool(text, ok);
            if (!ok) error = QString("expected true or false, got '%1'").arg(text);
            else result = v;
        } else error = QString("expected true or false");
        break;

    case ParamType::Date:
        if (!isString) {
            error = QString("expected a date (yyyy-mm-dd)");
        } else {
            QDate d = QDate::fromString(text.trimmed(), Qt::ISODate);
            if (!d.isValid()) error = QString("expected a date (yyyy-mm-dd), got '%1'").arg(text);
            else result = d.toString(Qt::ISODate);
        }
        break;
    }

    if (error.isEmpty() && !param.choices.isEmpty()) {
        QString v = result.toVariant().toString();
        bool found = false;
        for (const QString &c : param.choices) if (c.compare(v, Qt::CaseInsensitive) == 0) { result = c; found = true; }
        if (!found) error = QString("must be one of: %1").arg(param.choices.join(", "));
    }
    return result;
}

QString
CommandRegistry::validate(const CommandSpec &spec, QJsonObject &args)
{
    // unknown parameters are an error, a typo should never be silently ignored
    for (const QString &key : args.keys()) {
        if (!spec.param(key))
            return QString("unknown parameter '%1' for '%2'").arg(key).arg(spec.cliName());
    }

    QJsonObject out;
    for (const ParamSpec &p : spec.params) {

        if (!args.contains(p.name) || args.value(p.name).isNull()) {
            if (p.required) return QString("missing required parameter '%1'").arg(p.name);
            if (!p.defaultValue.isUndefined() && !p.defaultValue.isNull()) out.insert(p.name, p.defaultValue);
            continue;
        }

        QJsonValue value = args.value(p.name);
        QString error;

        if (p.repeated) {
            // accept a single value or an array
            QJsonArray in = value.isArray() ? value.toArray() : QJsonArray{ value };
            QJsonArray list;
            for (const QJsonValue &v : in) {
                QJsonValue c = coerce(p, v, error);
                if (!error.isEmpty()) return QString("parameter '%1': %2").arg(p.name).arg(error);
                list.append(c);
            }
            if (p.required && list.isEmpty()) return QString("missing required parameter '%1'").arg(p.name);
            out.insert(p.name, list);
        } else {
            if (value.isArray()) {
                QJsonArray a = value.toArray();
                if (a.count() != 1) return QString("parameter '%1' may only be given once").arg(p.name);
                value = a.first();
            }
            QJsonValue c = coerce(p, value, error);
            if (!error.isEmpty()) return QString("parameter '%1': %2").arg(p.name).arg(error);
            out.insert(p.name, c);
        }
    }
    args = out;
    return QString();
}

QJsonObject
CommandRegistry::describe(const CommandSpec &spec)
{
    QJsonObject o;
    o.insert("name", spec.name);
    o.insert("cli", spec.cliName());
    o.insert("summary", spec.summary);
    if (!spec.description.isEmpty()) o.insert("description", spec.description);
    o.insert("scope", spec.scope == Scope::Athlete ? "athlete" : "global");
    o.insert("modifies", spec.modifies);
    if (!spec.httpPath.isEmpty()) {
        o.insert("method", spec.httpMethod);
        o.insert("path", spec.httpPath);
    }
    QJsonArray params;
    for (const ParamSpec &p : spec.params) {
        QJsonObject po;
        po.insert("name", p.name);
        po.insert("type", paramTypeName(p.type));
        po.insert("description", p.description);
        po.insert("required", p.required);
        if (p.positional) po.insert("positional", true);
        if (p.repeated) po.insert("repeated", true);
        if (!p.defaultValue.isUndefined() && !p.defaultValue.isNull()) po.insert("default", p.defaultValue);
        if (!p.choices.isEmpty()) po.insert("choices", QJsonArray::fromStringList(p.choices));
        if (p.commandLine) po.insert("cli_only", true);
        if (p.uploads) po.insert("upload", true);
        params.append(po);
    }
    o.insert("params", params);
    return o;
}

} // namespace Headless
