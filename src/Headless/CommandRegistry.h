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

#ifndef _GC_CommandRegistry_h
#define _GC_CommandRegistry_h 1

#include "HeadlessCommand.h"

#include <QMap>

namespace Headless {

//
// Holds the command table and validates requests against it.
//
// Validation is shared by all entry points: arguments may arrive as strings
// (argv, URL query) or as typed JSON (REST body) and are coerced to the
// declared parameter types here, defaults are applied and unknown or
// missing parameters are reported the same way everywhere.
//
class CommandRegistry
{
    public:

        void add(const Command &command);

        const Command *find(const QString &name) const;
        QList<const Command *> commands() const;        // sorted by name
        QStringList names() const;

        // command names that start with the given dotted prefix
        // ("activity" -> activity.list, activity.show ...)
        QStringList namesWithPrefix(const QString &prefix) const;

        // coerce and validate args in place, returns an empty string when ok
        static QString validate(const CommandSpec &spec, QJsonObject &args);

        // coerce a single value to a parameter type, sets error on failure
        static QJsonValue coerce(const ParamSpec &param, const QJsonValue &value, QString &error);

        // machine readable description of a command (used by help and REST)
        static QJsonObject describe(const CommandSpec &spec);

    private:

        QMap<QString, Command> table;
};

} // namespace Headless

#endif
