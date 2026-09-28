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

#ifndef _GC_RestRouter_h
#define _GC_RestRouter_h 1

#include "CommandRegistry.h"

#include <QMultiMap>

namespace Headless {

//
// Maps HTTP requests onto commands, using the REST binding declared in each
// CommandSpec. Pure logic, no networking, so it is unit tested directly.
//
//   GET  /v1/athletes/Joe/activities?filter=isRun%3D0   -> activity.list
//   POST /v1/commands/activity.list  {"athlete":"Joe", "args":{...}}
//
class RestRouter
{
    public:

        static constexpr const char *prefix = "/v1";

        explicit RestRouter(const CommandRegistry &registry);

        struct Match {
            int httpStatus = 200;       // 404 no route, 405 wrong method, 400 bad body
            QString error;
            QString command;            // matched command
            QString athlete;            // from {athlete} or the body
            QJsonObject args;           // raw values, validated by the registry later
            QStringList allowed;        // methods allowed on the path (for 405)
        };

        // query values arrive as strings, repeated keys become arrays;
        // a JSON object body is merged over them with its own types
        Match match(const QString &method, const QString &path,
                    const QMultiMap<QString, QString> &query, const QByteArray &jsonBody) const;

        // OpenAPI 3 description of every route
        QJsonObject openApi(const QString &serverUrl) const;

    private:

        struct Route {
            QString method;
            QStringList segments;       // "athletes", "{athlete}", "activities"
            const Command *command;
        };

        static bool matchSegments(const QStringList &pattern, const QStringList &path, QMap<QString,QString> &params);

        const CommandRegistry &registry;
        QList<Route> routes;
};

} // namespace Headless

#endif
