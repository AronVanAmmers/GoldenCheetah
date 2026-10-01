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

//
// The favourite metrics that order the ride summary and interval list, and
// the activity list columns. The overview's intervals table is a layout
// tile program, see OverviewCommands.cpp.
//

#include "HeadlessCommands.h"
#include "MetricNames.h"

#include "Settings.h"
#include "RideMetric.h"

namespace Headless {

//
// Favourites: the ordered list the intervals table and ride summary walk.
//

static CommandResult
resolveSymbols(const QJsonArray &given, QStringList &symbols)
{
    for (const QJsonValue &v : given) {
        QString symbol = metricSymbol(v.toString());
        if (symbol.isEmpty())
            return CommandResult::failure(Status::Usage, QString("unknown metric '%1', see 'metric list'").arg(v.toString()));
        if (!symbols.contains(symbol)) symbols << symbol;
    }
    return CommandResult::success();
}

static QJsonObject
favouritesJson(const QStringList &symbols)
{
    QJsonObject data;
    data.insert("metrics", QJsonArray::fromStringList(symbols));
    return data;
}

static CommandResult
listFavourites(CommandEnvironment &, const CommandRequest &)
{
    return CommandResult::success(favouritesJson(favouriteMetrics()));
}

static CommandResult
addFavourites(CommandEnvironment &env, const CommandRequest &request)
{
    CommandResult writable = sharedSettingsWritable(env);
    if (!writable.ok()) return writable;

    QStringList symbols = favouriteMetrics();
    QStringList added;
    CommandResult resolved = resolveSymbols(request.args.value("symbol").toArray(), added);
    if (!resolved.ok()) return resolved;

    QStringList appended;
    for (const QString &symbol : added)
        if (!symbols.contains(symbol)) { symbols << symbol; appended << symbol; }
    if (!appended.isEmpty()) setFavouriteMetrics(symbols);

    QJsonObject data = favouritesJson(symbols);
    data.insert("added", QJsonArray::fromStringList(appended));
    return CommandResult::success(data);
}

static CommandResult
removeFavourites(CommandEnvironment &env, const CommandRequest &request)
{
    CommandResult writable = sharedSettingsWritable(env);
    if (!writable.ok()) return writable;

    QStringList drop;
    CommandResult resolved = resolveSymbols(request.args.value("symbol").toArray(), drop);
    if (!resolved.ok()) return resolved;

    QStringList symbols = favouriteMetrics();
    QStringList removed;
    for (const QString &symbol : drop) {
        if (!symbols.contains(symbol)) continue;
        symbols.removeAll(symbol);
        removed << symbol;
    }
    if (removed.isEmpty())
        return CommandResult::failure(Status::NotFound, QString("not a favourite: %1").arg(drop.join(", ")));
    setFavouriteMetrics(symbols);

    QJsonObject data = favouritesJson(symbols);
    data.insert("removed", QJsonArray::fromStringList(removed));
    return CommandResult::success(data);
}

static CommandResult
setFavourites(CommandEnvironment &env, const CommandRequest &request)
{
    CommandResult writable = sharedSettingsWritable(env);
    if (!writable.ok()) return writable;

    QStringList symbols;
    CommandResult resolved = resolveSymbols(request.args.value("symbol").toArray(), symbols);
    if (!resolved.ok()) return resolved;
    setFavouriteMetrics(symbols);
    return CommandResult::success(favouritesJson(symbols));
}

//
// Activity list columns. Stored as the GUI stores them: a heading is the
// metric's internal name, widths stay aligned with the headings.
//

static const QString defaultHeadings = "*|Workout Code|Date|";
static const QString defaultWidths = "0|100|100|";

static void
loadColumns(const QString &athlete, QStringList &columns, QStringList &widths)
{
    QString headings = appsettings->cvalue(athlete, GC_NAVHEADINGS, "").toString();
    QString stored = appsettings->cvalue(athlete, GC_NAVHEADINGWIDTHS, "").toString();
    if (headings.isEmpty()) {
        headings = defaultHeadings;
        stored = defaultWidths;
    }
    columns = headings.split("|", Qt::SkipEmptyParts);
    widths = stored.split("|", Qt::SkipEmptyParts);
    while (widths.count() < columns.count()) widths << "100";
    if (widths.count() > columns.count()) widths = widths.mid(0, columns.count());
}

static void
saveColumns(const QString &athlete, const QStringList &columns, const QStringList &widths)
{
    appsettings->setCValue(athlete, GC_NAVHEADINGS, columns.join("|") + "|");
    appsettings->setCValue(athlete, GC_NAVHEADINGWIDTHS, widths.join("|") + "|");
}

static const RideMetric *
metricForColumn(const QString &name)
{
    const RideMetricFactory &factory = RideMetricFactory::instance();
    QString symbol = metricSymbol(name);
    if (!symbol.isEmpty()) return factory.rideMetric(symbol);
    for (int i = 0; i < factory.metricCount(); i++) {
        const RideMetric *m = factory.rideMetric(factory.metricName(i));
        if (!m) continue;
        if (m->name() == name || m->internalName() == name || m->symbol() == name) return m;
    }
    return nullptr;
}

static QString
columnHeading(const RideMetric *metric)
{
    return metric->internalName();
}

static CommandResult
listColumns(CommandEnvironment &env, const CommandRequest &)
{
    QStringList columns, widths;
    loadColumns(env.session->name(), columns, widths);
    QJsonObject data;
    data.insert("columns", QJsonArray::fromStringList(columns));
    return CommandResult::success(data);
}

static CommandResult
addColumn(CommandEnvironment &env, const CommandRequest &request)
{
    const RideMetric *metric = metricForColumn(request.args.value("metric").toString());
    if (!metric)
        return CommandResult::failure(Status::Usage, QString("unknown metric '%1', see 'metric list'").arg(request.args.value("metric").toString()));

    QStringList columns, widths;
    loadColumns(env.session->name(), columns, widths);
    QString heading = columnHeading(metric);
    QString status = "unchanged";
    if (!columns.contains(heading) && !columns.contains(metric->name()) && !columns.contains(metric->symbol())) {
        columns << heading;
        widths << "100";
        saveColumns(env.session->name(), columns, widths);
        status = "added";
    }
    QJsonObject data;
    data.insert("status", status);
    data.insert("column", heading);
    data.insert("columns", QJsonArray::fromStringList(columns));
    return CommandResult::success(data);
}

static CommandResult
removeColumn(CommandEnvironment &env, const CommandRequest &request)
{
    const RideMetric *metric = metricForColumn(request.args.value("metric").toString());
    if (!metric)
        return CommandResult::failure(Status::Usage, QString("unknown metric '%1', see 'metric list'").arg(request.args.value("metric").toString()));

    QStringList columns, widths;
    loadColumns(env.session->name(), columns, widths);
    QStringList headings = { columnHeading(metric), metric->name(), metric->symbol() };
    int index = -1;
    for (int i = 0; i < columns.count(); i++)
        if (headings.contains(columns.at(i))) { index = i; break; }
    if (index < 0)
        return CommandResult::failure(Status::NotFound, QString("'%1' is not an activity column").arg(metric->name()));

    columns.removeAt(index);
    widths.removeAt(index);
    saveColumns(env.session->name(), columns, widths);

    QJsonObject data;
    data.insert("removed", metric->internalName());
    data.insert("columns", QJsonArray::fromStringList(columns));
    return CommandResult::success(data);
}

void
registerNavigatorCommands(CommandRegistry &registry)
{
    Command flist;
    flist.spec.name = "metric.favourite.list";
    flist.spec.summary = "list favourite metrics in the order the ride summary and interval list show them";
    flist.spec.description =
        "This is Preferences → Metrics → Favourites. The ride summary and\n"
        "'interval list' walk the list from first to last. The intervals table\n"
        "on the activity overview is a tile program: see 'layout tile show'.";
    flist.spec.scope = Scope::Athlete;
    flist.spec.httpMethod = "GET";
    flist.spec.httpPath = "/athletes/{athlete}/favourites";
    flist.handler = listFavourites;
    registry.add(flist);

    Command fadd;
    fadd.spec.name = "metric.favourite.add";
    fadd.spec.summary = "append metrics to the favourites, so they show at the bottom of the ride summary";
    fadd.spec.scope = Scope::Athlete;
    fadd.spec.modifies = true;
    fadd.spec.params << ParamSpec("symbol", ParamType::String, "metric symbol or formula name").req().pos().many();
    fadd.spec.httpMethod = "POST";
    fadd.spec.httpPath = "/athletes/{athlete}/favourites";
    fadd.handler = addFavourites;
    registry.add(fadd);

    Command fremove;
    fremove.spec.name = "metric.favourite.remove";
    fremove.spec.summary = "remove metrics from the favourites, leaving the rest in order";
    fremove.spec.scope = Scope::Athlete;
    fremove.spec.modifies = true;
    fremove.spec.params << ParamSpec("symbol", ParamType::String, "metric symbol or formula name").req().pos().many();
    fremove.spec.httpMethod = "DELETE";
    fremove.spec.httpPath = "/athletes/{athlete}/favourites/{symbol}";
    fremove.handler = removeFavourites;
    registry.add(fremove);

    Command fset;
    fset.spec.name = "metric.favourite.set";
    fset.spec.summary = "replace the favourites; argument order is the ride summary order";
    fset.spec.description =
        "The same list Preferences saves after you move rows with the up and\n"
        "down buttons. The first symbol is the first row of the ride summary\n"
        "and of 'interval list'. The overview's intervals table is separate.";
    fset.spec.scope = Scope::Athlete;
    fset.spec.modifies = true;
    fset.spec.params << ParamSpec("symbol", ParamType::String, "metric symbol or formula name, in display order").req().pos().many();
    fset.spec.httpMethod = "PUT";
    fset.spec.httpPath = "/athletes/{athlete}/favourites";
    fset.handler = setFavourites;
    registry.add(fset);

    Command clist;
    clist.spec.name = "activity.column.list";
    clist.spec.summary = "list the activity list columns";
    clist.spec.scope = Scope::Athlete;
    clist.spec.httpMethod = "GET";
    clist.spec.httpPath = "/athletes/{athlete}/columns";
    clist.handler = listColumns;
    registry.add(clist);

    Command cadd;
    cadd.spec.name = "activity.column.add";
    cadd.spec.summary = "add a metric column to the activity list";
    cadd.spec.scope = Scope::Athlete;
    cadd.spec.modifies = true;
    cadd.spec.params << ParamSpec("metric", ParamType::String, "metric symbol, formula name or column heading").req().pos();
    cadd.spec.httpMethod = "POST";
    cadd.spec.httpPath = "/athletes/{athlete}/columns";
    cadd.handler = addColumn;
    registry.add(cadd);

    Command cremove;
    cremove.spec.name = "activity.column.remove";
    cremove.spec.summary = "remove a metric column from the activity list";
    cremove.spec.scope = Scope::Athlete;
    cremove.spec.modifies = true;
    cremove.spec.params << ParamSpec("metric", ParamType::String, "metric symbol, formula name or column heading").req().pos();
    cremove.spec.httpMethod = "DELETE";
    cremove.spec.httpPath = "/athletes/{athlete}/columns/{metric}";
    cremove.handler = removeColumn;
    registry.add(cremove);
}

} // namespace Headless
