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

#ifndef _GC_ResultFormat_h
#define _GC_ResultFormat_h 1

#include "HeadlessCommand.h"

namespace Headless {

class ResultFormat
{
    public:

        // the JSON document both entry points return:
        // { "ok": true, "status": "ok", "command": "...", "data": {...},
        //   "warnings": [...], "error": "..." }
        static QJsonObject envelope(const QString &command, const CommandResult &result);

        // human readable rendering, the handler's own text when it has one,
        // otherwise a generic layout: tables for lists, key: value otherwise
        static QString text(const CommandResult &result);

        // generic rendering of a JSON object
        // `then` names columns to place after the usual identity columns
        // and before whatever remains, so a metric list keeps its order
        static QString render(const QJsonObject &data, const QStringList &then = QStringList());

        // a list of flat objects as an aligned table
        static QString table(const QJsonArray &rows, const QStringList &then = QStringList());

        // a scalar as text
        static QString scalar(const QJsonValue &v);

        // CSV: the handler's own when it has one, else a result that is one
        // list (besides plain values) is that list as a table, nested values
        // as columns as in the text table, and anything else is key,value
        // lines with dotted paths
        static QString csv(const CommandResult &result);
        static QString csv(const QJsonObject &data, const QStringList &then = QStringList());

        // one CSV line, fields quoted where needed (RFC 4180)
        static QString csvLine(const QStringList &fields);

        // a value in full precision for CSV
        static QString csvValue(const QJsonValue &v);

        // a batch command's line for one item: its status (padded to width),
        // its key field, anything after that, and the message if it has one
        static QString statusLine(const QJsonObject &item, const QString &keyField, int width,
                                  const QString &afterKey = QString());
};

} // namespace Headless

#endif
