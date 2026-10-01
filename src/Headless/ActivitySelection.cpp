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

#include "ActivitySelection.h"
#include "MetricNames.h"
#include "ActivityJson.h"
#include "AthleteSession.h"

#include "Context.h"
#include "Athlete.h"
#include "RideCache.h"
#include "RideItem.h"
#include "RideMetric.h"
#include "DataFilter.h"
#include "FreeSearch.h"
#include "NamedSearch.h"

#include <QSet>
#include <QHash>
#include <QFileInfo>
#include <cmath>

namespace Headless {

bool
ActivitySelection::isEmpty() const
{
    return activities.isEmpty() && filter.isEmpty() && search.isEmpty() && named.isEmpty()
        && !from.isValid() && !to.isValid() && sport.isEmpty() && limit == 0;
}

QList<ParamSpec>
ActivitySelection::params(bool positionalActivities)
{
    QList<ParamSpec> list;
    ParamSpec a("activity", ParamType::String,
                "activity file name, start time (yyyy-mm-ddThh:mm:ss), date, 'first' or 'last'");
    a.many();
    if (positionalActivities) a.pos();
    list << a;
    list << ParamSpec("filter", ParamType::String, "filter expression, as typed in the GUI filter box (e.g. 'isRun = 0')");
    list << ParamSpec("search", ParamType::String, "free text search of the activity metadata, as the GUI search box");
    list << ParamSpec("named", ParamType::String, "name of a search or filter saved in the GUI");
    list << ParamSpec("from", ParamType::Date, "only activities on or after this date");
    list << ParamSpec("to", ParamType::Date, "only activities on or before this date");
    list << ParamSpec("sport", ParamType::String, "only activities of this sport (Bike, Run, Swim ...)");
    list << ParamSpec("planned", ParamType::Bool, "planned instead of completed activities");
    list << ParamSpec("limit", ParamType::Int, "only the most recent N activities");
    return list;
}

ActivitySelection
ActivitySelection::fromArgs(const QJsonObject &args)
{
    ActivitySelection s;
    for (const QJsonValue &v : args.value("activity").toArray()) s.activities << v.toString();
    s.filter = args.value("filter").toString();
    s.search = args.value("search").toString();
    s.named = args.value("named").toString();
    if (args.contains("from")) s.from = QDate::fromString(args.value("from").toString(), Qt::ISODate);
    if (args.contains("to")) s.to = QDate::fromString(args.value("to").toString(), Qt::ISODate);
    s.sport = args.value("sport").toString();
    s.planned = args.value("planned").toBool(false);
    s.limit = args.value("limit").toInt(0);
    return s;
}

// filenames that pass a DataFilter expression
static bool
applyFilter(Context *context, const QList<RideItem*> &in, const QString &expression,
            QList<RideItem*> &out, QString &error)
{
    DataFilter df(nullptr, context, expression);
    if (!df.getErrors().isEmpty()) {
        error = QString("bad filter '%1': %2").arg(expression).arg(df.getErrors().join("; "));
        return false;
    }
    for (RideItem *item : in) {
        Result res = df.evaluate(item, nullptr);
        if (res.isNumber && res.number()) out << item;
    }
    return true;
}

static void
applySearch(Context *context, const QList<RideItem*> &in, const QString &text, QList<RideItem*> &out)
{
    FreeSearch fs;
    QSet<QString> hits;
    for (const QString &f : fs.search(context, text)) hits.insert(f);
    for (RideItem *item : in) if (hits.contains(item->fileName)) out << item;
}

bool
ActivitySelection::resolve(AthleteSession &session, QList<RideItem *> &result, QString &error, Status &status) const
{
    result.clear();
    error.clear();
    status = Status::Ok;

    Context *context = session.context();
    RideCache *cache = session.rideCache();

    QList<RideItem *> candidates;

    if (!activities.isEmpty()) {
        // named one by one, in the order given, no duplicates
        QSet<RideItem *> seen;
        for (const QString &id : activities) {
            QString why;
            RideItem *item = session.findActivity(id, why);
            if (!item) {
                error = why;
                status = Status::NotFound;
                return false;
            }
            if (!seen.contains(item)) candidates << item;
            seen.insert(item);
        }
    } else {
        for (RideItem *item : cache->rides()) if (item->planned == planned) candidates << item;
    }

    // simple criteria first, they are cheap
    QList<RideItem *> list;
    for (RideItem *item : candidates) {
        QDate d = item->dateTime.date();
        if (from.isValid() && d < from) continue;
        if (to.isValid() && d > to) continue;
        if (!sport.isEmpty() && item->sport.compare(sport, Qt::CaseInsensitive) != 0) continue;
        list << item;
    }

    if (!named.isEmpty()) {
        NamedSearch ns = NamedSearches::getInstance().get(named);
        if (ns.name.isEmpty()) {
            error = QString("no saved search or filter called '%1'").arg(named);
            status = Status::NotFound;
            return false;
        }
        QList<RideItem *> out;
        if (ns.type == NamedSearch::filter) {
            if (!applyFilter(context, list, ns.text, out, error)) { status = Status::Usage; return false; }
        } else {
            applySearch(context, list, ns.text, out);
        }
        list = out;
    }

    if (!filter.isEmpty()) {
        QList<RideItem *> out;
        if (!applyFilter(context, list, filter, out, error)) { status = Status::Usage; return false; }
        list = out;
    }

    if (!search.isEmpty()) {
        QList<RideItem *> out;
        applySearch(context, list, search, out);
        list = out;
    }

    if (limit > 0 && list.count() > limit) list = list.mid(list.count() - limit);

    result = list;
    return true;
}

QStringList
splitList(const QJsonValue &v)
{
    QJsonArray values = v.isArray() ? v.toArray() : (v.isString() ? QJsonArray{ v } : QJsonArray());
    QStringList list;
    for (const QJsonValue &x : values)
        for (const QString &part : x.toString().split(",", Qt::SkipEmptyParts)) list << part.trimmed();
    return list;
}

} // namespace Headless
