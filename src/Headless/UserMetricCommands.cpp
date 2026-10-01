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
// User metrics (Preferences → Metrics → Custom), the favourites that
// order the ride summary and interval list, and the activity list
// columns. The overview's intervals table is a layout tile program.
// Formulas are checked the way the editor checks them before anything
// is written.
//

#include "HeadlessCommands.h"
#include "ProgramArgs.h"
#include "ActivitySelection.h"

#include "Context.h"
#include "Settings.h"
#include "RideMetric.h"
#include "UserMetricParser.h"
#include "UserMetricSettings.h"
#include "DataFilter.h"

#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QXmlInputSource>
#include <QXmlSimpleReader>

extern QString gcroot;

namespace Headless {

static const QStringList metricTypes = { "total", "average", "peak", "low" };

static QString
typeName(int type)
{
    if (type >= 0 && type < metricTypes.count()) return metricTypes.at(type);
    return QString::number(type);
}

static QString
userMetricsFile()
{
    return QDir(gcroot).absoluteFilePath("usermetrics.xml");
}

static QList<UserMetricSettings>
loadUserMetrics()
{
    QList<UserMetricSettings> metrics;
    QString filename = userMetricsFile();
    if (!QFile::exists(filename)) return metrics;

    for (const UserMetricSettings &m : UserMetricParser::load(filename))
        if (!m.symbol.startsWith("compatibility_")) metrics << m;
    return metrics;
}

// all athletes share the file: a failed write must leave the old one
static bool
writeUserMetrics(const QList<UserMetricSettings> &metrics, QString &error)
{
    return UserMetricParser::serialize(userMetricsFile(), metrics, &error);
}

static void
metricsChanged(AthleteSession &s)
{
    GlobalContext::context()->notifyConfigChanged(CONFIG_USERMETRICS);
    invalidateMetricLookup();
    s.waitForRefresh();
}

static int
findMetric(const QList<UserMetricSettings> &metrics, const QString &symbol)
{
    for (int i = 0; i < metrics.count(); i++)
        if (metrics.at(i).symbol == symbol) return i;
    return -1;
}

static QJsonObject
metricJson(const UserMetricSettings &m, bool withProgram)
{
    QJsonObject o;
    o.insert("symbol", m.symbol);
    o.insert("name", m.name);
    o.insert("type", typeName(m.type));
    o.insert("units", m.unitsMetric);
    o.insert("imperial_units", m.unitsImperial);
    o.insert("conversion", m.conversion);
    o.insert("conversion_sum", m.conversionSum);
    o.insert("precision", m.precision);
    if (!m.description.isEmpty()) o.insert("description", m.description);
    if (m.istime) o.insert("time", true);
    if (m.aggzero) o.insert("aggregate_zero", true);
    if (withProgram) o.insert("program", m.program);
    return o;
}

static CommandResult
checkSymbol(const QString &symbol, const QString &keeping)
{
    if (symbol.isEmpty()) return CommandResult::failure(Status::Usage, "symbol is empty");
    const RideMetric *existing = RideMetricFactory::instance().rideMetric(symbol);
    if (existing && !existing->isUser())
        return CommandResult::failure(Status::Usage, QString("symbol '%1' is already a builtin metric").arg(symbol));
    if (existing && existing->isUser() && symbol != keeping)
        return CommandResult::failure(Status::Usage, QString("a user metric called '%1' already exists, use 'metric user edit'").arg(symbol));
    return CommandResult::success();
}

static CommandResult
checkName(const QString &name)
{
    if (name.isEmpty()) return CommandResult::failure(Status::Usage, "name is empty");
    if (name.contains('_')) return CommandResult::failure(Status::Usage, "name can't contain '_'");
    return CommandResult::success();
}

static void
stamp(UserMetricSettings &m)
{
    QString program = m.program;
    m.fingerprint = m.symbol + DataFilter::fingerprint(program);
}

static CommandResult
listUserMetrics(CommandEnvironment &, const CommandRequest &)
{
    QJsonArray list;
    for (const UserMetricSettings &m : loadUserMetrics()) list.append(metricJson(m, false));
    QJsonObject data;
    data.insert("metrics", list);
    data.insert("file", userMetricsFile());
    return CommandResult::success(data);
}

static CommandResult
showUserMetric(CommandEnvironment &, const CommandRequest &request)
{
    QString symbol = request.args.value("symbol").toString();
    QList<UserMetricSettings> metrics = loadUserMetrics();
    int i = findMetric(metrics, symbol);
    if (i < 0) return CommandResult::failure(Status::NotFound, QString("no user metric '%1'").arg(symbol));
    return CommandResult::success(metricJson(metrics.at(i), true));
}

static bool
applyFields(const CommandRequest &request, UserMetricSettings &m, bool creating, QString &error)
{
    if (request.args.contains("name")) m.name = request.args.value("name").toString().trimmed();
    else if (creating) m.name.clear();

    if (request.args.contains("type")) {
        int t = metricTypes.indexOf(request.args.value("type").toString());
        if (t < 0) { error = "type must be total, average, peak or low"; return false; }
        m.type = t;
    } else if (creating) m.type = RideMetric::Average;

    if (request.args.contains("units")) m.unitsMetric = request.args.value("units").toString();
    if (request.args.contains("imperial-units")) m.unitsImperial = request.args.value("imperial-units").toString();
    if (request.args.contains("conversion")) m.conversion = request.args.value("conversion").toDouble();
    else if (creating) m.conversion = 1;
    if (request.args.contains("conversion-sum")) m.conversionSum = request.args.value("conversion-sum").toDouble();
    else if (creating) m.conversionSum = 0;
    if (request.args.contains("precision")) {
        int p = request.args.value("precision").toInt();
        if (p < 0) { error = "precision can't be negative"; return false; }
        m.precision = p;
    } else if (creating) m.precision = 0;
    if (request.args.contains("description")) m.description = request.args.value("description").toString();
    if (request.args.contains("time")) m.istime = request.args.value("time").toBool();
    if (request.args.contains("aggregate-zero")) m.aggzero = request.args.value("aggregate-zero").toBool();
    return true;
}

static CommandResult
addUserMetric(CommandEnvironment &env, const CommandRequest &request)
{
    CommandResult writable = sharedSettingsWritable(env);
    if (!writable.ok()) return writable;

    UserMetricSettings m;
    m.symbol = request.args.value("symbol").toString().trimmed();
    QString error;
    if (!applyFields(request, m, true, error)) return CommandResult::failure(Status::Usage, error);

    CommandResult symbol = checkSymbol(m.symbol, QString());
    if (!symbol.ok()) return symbol;
    CommandResult name = checkName(m.name);
    if (!name.ok()) return name;

    CommandResult program = readProgramArg(request, true, m.program);
    if (!program.ok()) return program;
    CommandResult compiled = checkProgram(env.session->context(), m.program, true);
    if (!compiled.ok()) return compiled;

    QList<UserMetricSettings> metrics = loadUserMetrics();
    if (findMetric(metrics, m.symbol) >= 0)
        return CommandResult::failure(Status::Usage, QString("a user metric called '%1' already exists, use 'metric user edit'").arg(m.symbol));

    stamp(m);
    metrics.append(m);
    if (!writeUserMetrics(metrics, error)) return CommandResult::failure(Status::Failed, error);
    metricsChanged(*env.session);

    QJsonObject data = metricJson(m, true);
    data.insert("status", "added");
    data.insert("refreshed", env.session->rideCache()->lastStaleCount());
    CommandResult result = CommandResult::success(data);
    result.text = QString("added user metric %1, %2 activities recomputed\n").arg(m.symbol).arg(data.value("refreshed").toInt());
    return result;
}

static CommandResult
editUserMetric(CommandEnvironment &env, const CommandRequest &request)
{
    CommandResult writable = sharedSettingsWritable(env);
    if (!writable.ok()) return writable;

    QString symbol = request.args.value("symbol").toString();
    QList<UserMetricSettings> metrics = loadUserMetrics();
    int i = findMetric(metrics, symbol);
    if (i < 0) return CommandResult::failure(Status::NotFound, QString("no user metric '%1'").arg(symbol));

    UserMetricSettings m = metrics.at(i);
    QString renamed = request.args.contains("rename") ? request.args.value("rename").toString().trimmed() : m.symbol;
    QString error;
    if (!applyFields(request, m, false, error)) return CommandResult::failure(Status::Usage, error);
    m.symbol = renamed;

    CommandResult symbolOk = checkSymbol(m.symbol, symbol);
    if (!symbolOk.ok()) return symbolOk;
    CommandResult name = checkName(m.name);
    if (!name.ok()) return name;

    QString program;
    CommandResult read = readProgramArg(request, false, program);
    if (!read.ok()) return read;
    if (request.args.contains("program") || request.args.contains("file")) {
        CommandResult compiled = checkProgram(env.session->context(), program, true);
        if (!compiled.ok()) return compiled;
        m.program = program;
    }

    stamp(m);
    metrics[i] = m;
    if (!writeUserMetrics(metrics, error)) return CommandResult::failure(Status::Failed, error);
    metricsChanged(*env.session);

    QJsonObject data = metricJson(m, true);
    data.insert("status", "updated");
    data.insert("refreshed", env.session->rideCache()->lastStaleCount());
    CommandResult result = CommandResult::success(data);
    result.text = QString("updated user metric %1, %2 activities recomputed\n").arg(m.symbol).arg(data.value("refreshed").toInt());
    return result;
}

static CommandResult
removeUserMetric(CommandEnvironment &env, const CommandRequest &request)
{
    CommandResult writable = sharedSettingsWritable(env);
    if (!writable.ok()) return writable;

    QString symbol = request.args.value("symbol").toString();
    QList<UserMetricSettings> metrics = loadUserMetrics();
    int i = findMetric(metrics, symbol);
    if (i < 0) return CommandResult::failure(Status::NotFound, QString("no user metric '%1'").arg(symbol));
    metrics.removeAt(i);

    QString error;
    if (!writeUserMetrics(metrics, error)) return CommandResult::failure(Status::Failed, error);
    metricsChanged(*env.session);

    QJsonObject data;
    data.insert("removed", symbol);
    data.insert("refreshed", env.session->rideCache()->lastStaleCount());
    CommandResult result = CommandResult::success(data);
    result.text = QString("removed user metric %1, %2 activities recomputed\n").arg(symbol).arg(data.value("refreshed").toInt());
    return result;
}

//
// Favourites: the ordered list the intervals table and ride summary walk.
//

static QStringList
favouriteSymbols()
{
    QString s = appsettings->contains(GC_SETTINGS_FAVOURITE_METRICS)
                ? appsettings->value(nullptr, GC_SETTINGS_FAVOURITE_METRICS).toString()
                : QString(GC_SETTINGS_FAVOURITE_METRICS_DEFAULT);
    if (s.trimmed().isEmpty()) s = GC_SETTINGS_FAVOURITE_METRICS_DEFAULT;

    QStringList symbols;
    for (const QString &part : s.split(",", Qt::SkipEmptyParts)) symbols << part.trimmed();
    return symbols;
}

static void
saveFavourites(const QStringList &symbols)
{
    appsettings->setValue(GC_SETTINGS_FAVOURITE_METRICS, symbols.join(","));
}

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
    return CommandResult::success(favouritesJson(favouriteSymbols()));
}

static CommandResult
addFavourites(CommandEnvironment &env, const CommandRequest &request)
{
    CommandResult writable = sharedSettingsWritable(env);
    if (!writable.ok()) return writable;

    QStringList symbols = favouriteSymbols();
    QStringList added;
    CommandResult resolved = resolveSymbols(request.args.value("symbol").toArray(), added);
    if (!resolved.ok()) return resolved;

    QStringList appended;
    for (const QString &symbol : added)
        if (!symbols.contains(symbol)) { symbols << symbol; appended << symbol; }
    if (!appended.isEmpty()) saveFavourites(symbols);

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

    QStringList symbols = favouriteSymbols();
    QStringList removed;
    for (const QString &symbol : drop) {
        if (!symbols.contains(symbol)) continue;
        symbols.removeAll(symbol);
        removed << symbol;
    }
    if (removed.isEmpty())
        return CommandResult::failure(Status::NotFound, QString("not a favourite: %1").arg(drop.join(", ")));
    saveFavourites(symbols);

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
    saveFavourites(symbols);
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

static ParamSpec
programParam()
{
    return ParamSpec("program", ParamType::String, "the formula, with relevant, value and count blocks");
}

static ParamSpec
fileParam()
{
    return programFileParam("file containing the formula, or - for stdin");
}

void
registerUserMetricCommands(CommandRegistry &registry)
{
    Command list;
    list.spec.name = "metric.user.list";
    list.spec.summary = "list user-defined metrics (Preferences → Metrics → Custom)";
    list.spec.scope = Scope::Athlete;
    list.spec.httpMethod = "GET";
    list.spec.httpPath = "/athletes/{athlete}/user-metrics";
    list.handler = listUserMetrics;
    registry.add(list);

    Command show;
    show.spec.name = "metric.user.show";
    show.spec.summary = "show one user-defined metric, including its formula";
    show.spec.scope = Scope::Athlete;
    show.spec.params << ParamSpec("symbol", ParamType::String, "metric symbol").req().pos();
    show.spec.httpMethod = "GET";
    show.spec.httpPath = "/athletes/{athlete}/user-metrics/{symbol}";
    show.handler = showUserMetric;
    registry.add(show);

    Command add;
    add.spec.name = "metric.user.add";
    add.spec.summary = "add a user-defined metric and recompute activities";
    add.spec.description =
        "The formula is checked before it is saved, the same checks the formula\n"
        "editor runs. A program that does not parse, or has no value block, is\n"
        "refused and the file is left unchanged. Saving rebuilds the metric cache.\n"
        "The definition is shared by every athlete in the athletes folder.\n"
        "Type average with count Duration makes a trend a time-weighted mean.";
    add.spec.scope = Scope::Athlete;
    add.spec.modifies = true;
    add.spec.params << ParamSpec("symbol", ParamType::String, "metric symbol, e.g. hrr_v").req().pos();
    add.spec.params << ParamSpec("name", ParamType::String, "name shown in the GUI").req();
    add.spec.params << ParamSpec("type", ParamType::String, "how several activities combine").oneOf(metricTypes);
    add.spec.params << ParamSpec("units", ParamType::String, "metric units, e.g. bpm/kph");
    add.spec.params << ParamSpec("imperial-units", ParamType::String, "imperial units, e.g. bpm/mph");
    add.spec.params << ParamSpec("conversion", ParamType::Double, "multiply the metric value for imperial display");
    add.spec.params << ParamSpec("conversion-sum", ParamType::Double, "add this after the conversion");
    add.spec.params << ParamSpec("precision", ParamType::Int, "digits after the decimal point");
    add.spec.params << ParamSpec("description", ParamType::String, "description");
    add.spec.params << ParamSpec("time", ParamType::Bool, "the value is a time");
    add.spec.params << ParamSpec("aggregate-zero", ParamType::Bool, "include zeros when aggregating");
    add.spec.params << programParam();
    add.spec.params << fileParam();
    add.spec.httpMethod = "POST";
    add.spec.httpPath = "/athletes/{athlete}/user-metrics";
    add.handler = addUserMetric;
    registry.add(add);

    Command edit;
    edit.spec.name = "metric.user.edit";
    edit.spec.summary = "change a user-defined metric and recompute activities";
    edit.spec.description =
        "Only the fields you pass are changed. A new formula is checked before\n"
        "it is saved; a bad one leaves the metric as it was. --rename changes\n"
        "the symbol.";
    edit.spec.scope = Scope::Athlete;
    edit.spec.modifies = true;
    edit.spec.params << ParamSpec("symbol", ParamType::String, "metric symbol").req().pos();
    edit.spec.params << ParamSpec("rename", ParamType::String, "new symbol");
    edit.spec.params << ParamSpec("name", ParamType::String, "name shown in the GUI");
    edit.spec.params << ParamSpec("type", ParamType::String, "how several activities combine").oneOf(metricTypes);
    edit.spec.params << ParamSpec("units", ParamType::String, "metric units, e.g. bpm/kph");
    edit.spec.params << ParamSpec("imperial-units", ParamType::String, "imperial units, e.g. bpm/mph");
    edit.spec.params << ParamSpec("conversion", ParamType::Double, "multiply the metric value for imperial display");
    edit.spec.params << ParamSpec("conversion-sum", ParamType::Double, "add this after the conversion");
    edit.spec.params << ParamSpec("precision", ParamType::Int, "digits after the decimal point");
    edit.spec.params << ParamSpec("description", ParamType::String, "description");
    edit.spec.params << ParamSpec("time", ParamType::Bool, "the value is a time");
    edit.spec.params << ParamSpec("aggregate-zero", ParamType::Bool, "include zeros when aggregating");
    edit.spec.params << programParam();
    edit.spec.params << fileParam();
    edit.spec.httpMethod = "PATCH";
    edit.spec.httpPath = "/athletes/{athlete}/user-metrics/{symbol}";
    edit.handler = editUserMetric;
    registry.add(edit);

    Command remove;
    remove.spec.name = "metric.user.remove";
    remove.spec.summary = "remove a user-defined metric and recompute activities";
    remove.spec.scope = Scope::Athlete;
    remove.spec.modifies = true;
    remove.spec.params << ParamSpec("symbol", ParamType::String, "metric symbol").req().pos();
    remove.spec.httpMethod = "DELETE";
    remove.spec.httpPath = "/athletes/{athlete}/user-metrics/{symbol}";
    remove.handler = removeUserMetric;
    registry.add(remove);

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
