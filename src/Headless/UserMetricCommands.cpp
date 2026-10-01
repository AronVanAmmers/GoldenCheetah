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
// User metrics (Preferences → Metrics → Custom). Formulas are checked the
// way the editor checks them before anything is written. The favourites
// and the activity list columns are in NavigatorCommands.cpp.
//

#include "HeadlessCommands.h"
#include "ProgramArgs.h"
#include "ActivitySelection.h"
#include "MetricNames.h"

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
    m.fingerprint = m.symbol + DataFilter::fingerprint(m.program);
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
}

} // namespace Headless
