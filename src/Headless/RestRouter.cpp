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

#include "RestRouter.h"

#include <QJsonDocument>
#include <QSet>
#include <algorithm>

namespace Headless {

static QStringList
splitPath(const QString &path)
{
    return path.split('/', Qt::SkipEmptyParts);
}

RestRouter::RestRouter(const CommandRegistry &registry) : registry(registry)
{
    for (const Command *c : registry.commands()) {
        if (c->spec.httpPath.isEmpty()) continue;
        Route r;
        r.method = c->spec.httpMethod.toUpper();
        r.segments = splitPath(c->spec.httpPath);
        r.command = c;
        routes << r;
    }

    // literal segments win over parameters: /cp/estimates before /cp/{x}
    std::stable_sort(routes.begin(), routes.end(), [](const Route &a, const Route &b) {
        int la = 0, lb = 0;
        for (const QString &s : a.segments) if (!s.startsWith("{")) la++;
        for (const QString &s : b.segments) if (!s.startsWith("{")) lb++;
        return la > lb;
    });
}

bool
RestRouter::matchSegments(const QStringList &pattern, const QStringList &path, QMap<QString,QString> &params)
{
    if (pattern.count() != path.count()) return false;
    QMap<QString,QString> found;
    for (int i = 0; i < pattern.count(); i++) {
        const QString &p = pattern.at(i);
        if (p.startsWith("{") && p.endsWith("}")) {
            if (path.at(i).isEmpty()) return false;
            found.insert(p.mid(1, p.length() - 2), path.at(i));
        } else if (p != path.at(i)) {
            return false;
        }
    }
    params = found;
    return true;
}

static void
addQuery(QJsonObject &args, const QString &key, const QString &value)
{
    if (!args.contains(key)) {
        args.insert(key, value);
        return;
    }
    QJsonValue existing = args.value(key);
    QJsonArray list = existing.isArray() ? existing.toArray() : QJsonArray{ existing };
    list.append(value);
    args.insert(key, list);
}

// an empty body is an empty object
static bool
parseBody(const QByteArray &body, QJsonObject &object, QString &error)
{
    object = QJsonObject();
    if (body.trimmed().isEmpty()) return true;
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error != QJsonParseError::NoError) {
        error = QString("request body is not valid JSON: %1 at offset %2").arg(pe.errorString()).arg(pe.offset);
        return false;
    }
    if (!doc.isObject()) {
        error = "request body must be a JSON object";
        return false;
    }
    object = doc.object();
    return true;
}

static bool
mergeBody(QJsonObject &args, const QByteArray &body, QString &error)
{
    QJsonObject o;
    if (!parseBody(body, o, error)) return false;
    for (const QString &k : o.keys()) args.insert(k, o.value(k));
    return true;
}

// a file named in a request is read where the server runs: the server's
// files are not the client's to read, and "-" would wait on its stdin
static bool
commandLineOnly(const CommandSpec &spec, const QJsonObject &args, RestRouter::Match &m)
{
    for (const QString &key : args.keys()) {
        const ParamSpec *p = spec.param(key);
        if (!p || !p->commandLine) continue;
        m.httpStatus = 400;
        m.error = QString("'%1' reads a file where the server runs, it is only for the command line; "
                          "send the content in the request instead").arg(key);
        return true;
    }
    return false;
}

RestRouter::Match
RestRouter::match(const QString &method, const QString &fullPath,
                  const QMultiMap<QString, QString> &query, const QByteArray &jsonBody) const
{
    Match m;
    QString path = fullPath;
    if (!path.startsWith(prefix)) {
        m.httpStatus = 404;
        m.error = QString("unknown path, the API lives under %1").arg(prefix);
        return m;
    }
    QStringList segments = splitPath(path.mid(QString(prefix).length()));
    QString verb = method.toUpper();

    QJsonObject args;
    for (auto it = query.constBegin(); it != query.constEnd(); ++it) addQuery(args, it.key(), it.value());

    // generic: POST /v1/commands/<name> {"athlete": "...", "args": {...}}
    if (segments.count() == 2 && segments.first() == "commands") {
        if (verb != "POST") {
            m.httpStatus = 405;
            m.allowed = QStringList{ "POST" };
            m.error = "use POST to run a command";
            return m;
        }
        const Command *c = registry.find(segments.at(1));
        if (!c) {
            m.httpStatus = 404;
            m.error = QString("unknown command '%1'").arg(segments.at(1));
            return m;
        }
        // {"athlete": ..., "args": {...}} and nothing else: a misspelt key
        // must not be dropped without a word
        QJsonObject body;
        QString error;
        if (!parseBody(jsonBody, body, error)) {
            m.httpStatus = 400;
            m.error = error;
            return m;
        }
        for (const QString &k : body.keys()) {
            if (k == "athlete" || k == "args") continue;
            m.httpStatus = 400;
            m.error = QString("unknown key '%1' in the request body, it takes \"athlete\" and \"args\"").arg(k);
            return m;
        }
        if (body.contains("args") && !body.value("args").isObject()) {
            m.httpStatus = 400;
            m.error = "\"args\" must be a JSON object of the command's parameters";
            return m;
        }
        m.command = c->spec.name;
        m.athlete = body.value("athlete").toString(args.value("athlete").toString());
        args.remove("athlete");
        QJsonObject bodyArgs = body.value("args").toObject();
        for (const QString &k : bodyArgs.keys()) args.insert(k, bodyArgs.value(k));
        if (commandLineOnly(c->spec, args, m)) return m;
        m.args = args;
        return m;
    }

    QStringList allowed;
    for (const Route &r : routes) {
        QMap<QString,QString> params;
        if (!matchSegments(r.segments, segments, params)) continue;
        if (r.method != verb) {
            if (!allowed.contains(r.method)) allowed << r.method;
            continue;
        }

        QString error;
        if (!mergeBody(args, jsonBody, error)) {
            m.httpStatus = 400;
            m.error = error;
            return m;
        }
        for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
            if (it.key() == "athlete") {
                m.athlete = it.value();
                continue;
            }
            const ParamSpec *p = r.command->spec.param(it.key());
            if (p && p->repeated) args.insert(it.key(), QJsonArray{ it.value() });
            else args.insert(it.key(), it.value());
        }
        // athlete may also be given as a query parameter on global routes
        if (m.athlete.isEmpty() && args.contains("athlete") && !r.command->spec.param("athlete")) {
            m.athlete = args.value("athlete").toString();
            args.remove("athlete");
        }
        if (commandLineOnly(r.command->spec, args, m)) return m;
        m.command = r.command->spec.name;
        m.args = args;
        return m;
    }

    if (!allowed.isEmpty()) {
        m.httpStatus = 405;
        m.allowed = allowed;
        m.error = QString("method %1 not allowed, use %2").arg(verb).arg(allowed.join(", "));
        return m;
    }
    m.httpStatus = 404;
    m.error = QString("no such resource: %1").arg(fullPath);
    return m;
}

static QJsonObject
schemaFor(const ParamSpec &p)
{
    QJsonObject s;
    switch (p.type) {
    case ParamType::Int: s.insert("type", "integer"); break;
    case ParamType::Double: s.insert("type", "number"); break;
    case ParamType::Bool: s.insert("type", "boolean"); break;
    case ParamType::Date: s.insert("type", "string"); s.insert("format", "date"); break;
    default: s.insert("type", "string"); break;
    }
    if (!p.choices.isEmpty()) s.insert("enum", QJsonArray::fromStringList(p.choices));
    if (!p.defaultValue.isUndefined() && !p.defaultValue.isNull()) s.insert("default", p.defaultValue);
    if (p.repeated) {
        QJsonObject a;
        a.insert("type", "array");
        a.insert("items", s);
        return a;
    }
    return s;
}

QJsonObject
RestRouter::openApi(const QString &serverUrl) const
{
    QJsonObject paths;
    for (const Route &r : routes) {
        const CommandSpec &spec = r.command->spec;
        QString path = QString(prefix) + spec.httpPath;

        QJsonArray parameters;
        QSet<QString> inPath;
        for (const QString &seg : r.segments) {
            if (!seg.startsWith("{")) continue;
            QString name = seg.mid(1, seg.length() - 2);
            inPath.insert(name);
            QJsonObject p;
            p.insert("name", name);
            p.insert("in", "path");
            p.insert("required", true);
            p.insert("schema", QJsonObject{ { "type", "string" } });
            if (name == "athlete") p.insert("description", "athlete folder name");
            parameters.append(p);
        }

        bool body = r.method != "GET" && r.method != "DELETE";
        QJsonObject bodyProps;
        QJsonArray bodyRequired;
        for (const ParamSpec &ps : spec.params) {
            if (inPath.contains(ps.name) || ps.commandLine) continue;
            if (body) {
                QJsonObject s = schemaFor(ps);
                s.insert("description", ps.description);
                bodyProps.insert(ps.name, s);
                if (ps.required) bodyRequired.append(ps.name);
            } else {
                QJsonObject p;
                p.insert("name", ps.name);
                p.insert("in", "query");
                p.insert("required", ps.required);
                p.insert("description", ps.description);
                p.insert("schema", schemaFor(ps));
                if (ps.repeated) p.insert("explode", true);
                parameters.append(p);
            }
        }

        QJsonObject op;
        op.insert("operationId", spec.name);
        op.insert("summary", spec.summary);
        if (!spec.description.isEmpty()) op.insert("description", spec.description);
        op.insert("tags", QJsonArray{ spec.name.section('.', 0, 0) });
        if (!parameters.isEmpty()) op.insert("parameters", parameters);
        if (body && !bodyProps.isEmpty()) {
            QJsonObject schema;
            schema.insert("type", "object");
            schema.insert("properties", bodyProps);
            if (!bodyRequired.isEmpty()) schema.insert("required", bodyRequired);
            op.insert("requestBody", QJsonObject{
                { "content", QJsonObject{ { "application/json", QJsonObject{ { "schema", schema } } } } } });
        }
        QJsonObject responses;
        responses.insert("200", QJsonObject{ { "description", "ok: {ok, status, command, data}, or the file for charts and exports" } });
        responses.insert("400", QJsonObject{ { "description", "bad parameters" } });
        responses.insert("404", QJsonObject{ { "description", "athlete, activity or other resource not found" } });
        responses.insert("409", QJsonObject{ { "description", "athlete in use by another GoldenCheetah" } });
        op.insert("responses", responses);

        QJsonObject item = paths.value(path).toObject();
        item.insert(r.method.toLower(), op);
        paths.insert(path, item);
    }

    QJsonObject doc;
    doc.insert("openapi", "3.0.3");
    doc.insert("info", QJsonObject{
        { "title", "GoldenCheetah API" },
        { "version", "1" },
        { "description", "Headless GoldenCheetah: the same commands as 'GoldenCheetah --cli'." } });
    doc.insert("servers", QJsonArray{ QJsonObject{ { "url", serverUrl } } });
    doc.insert("paths", paths);
    return doc;
}

} // namespace Headless
