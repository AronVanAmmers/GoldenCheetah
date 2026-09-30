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
// Data processors ("fix" tools): the built in ones and Python processors
// (Edit > Python Fixes in the GUI). Running a processor on activities and
// saving them is the headless equivalent of Activity > Batch Processing.
//

#include "HeadlessCommands.h"
#include "HeadlessApp.h"
#include "ActivitySelection.h"

#include "Context.h"
#include "Athlete.h"
#include "RideCache.h"
#include "RideItem.h"
#include "RideFile.h"
#include "DataProcessor.h"
#include "Settings.h"
#include "JsonRideFile.h"

#ifdef GC_WANT_PYTHON
#include "FixPySettings.h"
#include "FixPyRunner.h"
#include "PythonEmbed.h"
#endif

#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace Headless {

static const QStringList automationNames = { "manual", "import", "save" };

static QString
automationName(DataProcessor::Automation a)
{
    switch (a) {
    case DataProcessor::Manual: return "manual";
    case DataProcessor::Auto: return "import";
    case DataProcessor::Save: return "save";
    }
    return "manual";
}

static DataProcessor::Automation
automationFromName(const QString &name)
{
    if (name == "import") return DataProcessor::Auto;
    if (name == "save") return DataProcessor::Save;
    return DataProcessor::Manual;
}

#ifdef GC_WANT_PYTHON
static void
loadPythonProcessors()
{
    // reading the scripts registers them with the processor factory
    if (fixPySettings) fixPySettings->getScripts();
}
#else
static void loadPythonProcessors() {}
#endif

// find by id, or by the (localised) name shown in the GUI
static DataProcessor *
findProcessor(const QString &name)
{
    loadPythonProcessors();
    DataProcessorFactory &factory = DataProcessorFactory::instance();
    DataProcessor *dp = factory.getProcessor(name);
    if (dp) return dp;
    // built in ids look like "::FixSpikes", accept "fixspikes" too
    for (DataProcessor *p : factory.getProcessors().values()) {
        QString id = p->id();
        if (id.startsWith("::")) id = id.mid(2);
        if (p->name().compare(name, Qt::CaseInsensitive) == 0 || id.compare(name, Qt::CaseInsensitive) == 0
            || p->id().compare(name, Qt::CaseInsensitive) == 0) return p;
    }
    return nullptr;
}

static QJsonObject
processorJson(DataProcessor *dp, bool withSource)
{
    QJsonObject o;
    o.insert("id", dp->id());
    o.insert("name", dp->name());
    o.insert("type", dp->isCoreProcessor() ? "builtin" : "python");
    o.insert("automation", automationName(dp->getAutomation()));
    if (dp->isAutomatedOnly()) o.insert("automated_only", true);
    o.insert("description", dp->explain().trimmed());
#ifdef GC_WANT_PYTHON
    if (!dp->isCoreProcessor() && fixPySettings) {
        FixPyScript *script = fixPySettings->getScript(dp->id());
        if (script) {
            o.insert("file", QDir(HeadlessApp::home()).absoluteFilePath(".pyfixes/" + script->path));
            if (withSource) o.insert("source", script->source);
        }
    }
#else
    Q_UNUSED(withSource);
#endif
    return o;
}

static CommandResult
listProcessors(CommandEnvironment &env, const CommandRequest &request)
{
    CommandResult home = requireHome(env);
    if (!home.ok()) return home;

    loadPythonProcessors();
    QString type = request.args.value("type").toString();
    QJsonArray list;
    for (DataProcessor *dp : DataProcessorFactory::instance().getProcessorsSorted()) {
        if (type == "builtin" && !dp->isCoreProcessor()) continue;
        if (type == "python" && dp->isCoreProcessor()) continue;
        QJsonObject o = processorJson(dp, false);
        o.remove("description");
        list.append(o);
    }
    QJsonObject data;
    data.insert("processors", list);
    data.insert("python", HeadlessApp::pythonAvailable());
    return CommandResult::success(data);
}

static CommandResult
showProcessor(CommandEnvironment &env, const CommandRequest &request)
{
    CommandResult home = requireHome(env);
    if (!home.ok()) return home;

    QString name = request.args.value("name").toString();
    DataProcessor *dp = findProcessor(name);
    if (!dp) return CommandResult::failure(Status::NotFound, QString("no data processor called '%1'").arg(name));
    return CommandResult::success(processorJson(dp, true));
}

static CommandResult
installProcessor(CommandEnvironment &env, const CommandRequest &request)
{
#ifdef GC_WANT_PYTHON
    CommandResult writable = sharedSettingsWritable(env);
    if (!writable.ok()) return writable;

    if (!appsettings->value(nullptr, GC_EMBED_PYTHON, true).toBool())
        return CommandResult::failure(Status::Failed, "embedded Python is switched off in the GoldenCheetah options");

    QString name = request.args.value("name").toString().trimmed();
    QString file = request.args.value("file").toString();
    QString source = request.args.value("source").toString();
    bool replace = request.args.value("replace").toBool(false);

    static const QString invalid = "<>:\"/\\|?*";
    for (QChar c : invalid)
        if (name.contains(c)) return CommandResult::failure(Status::Usage, QString("processor names can't contain %1").arg(invalid));
    if (name.isEmpty()) return CommandResult::failure(Status::Usage, "processor name is empty");

    if (!file.isEmpty()) {
        QFile in(file);
        if (!in.open(QIODevice::ReadOnly | QIODevice::Text))
            return CommandResult::failure(Status::NotFound, QString("can't read %1").arg(file));
        source = QTextStream(&in).readAll();
    }
    if (source.trimmed().isEmpty()) return CommandResult::failure(Status::Usage, "give the script with --file or --source");

    loadPythonProcessors();

    DataProcessor *existing = DataProcessorFactory::instance().getProcessor(name);
    if (existing && existing->isCoreProcessor())
        return CommandResult::failure(Status::Failed, QString("'%1' is a built in processor").arg(name));

    QString status;
    FixPyScript *script = fixPySettings->getScript(name);
    if (script) {
        if (script->source == source) {
            status = "unchanged";
        } else if (!replace) {
            return CommandResult::failure(Status::Failed,
                        QString("a Python processor called '%1' exists with a different script, use --replace").arg(name));
        } else {
            script->source = source;
            script->changed = true;
            status = "replaced";
        }
    } else {
        script = fixPySettings->createScript(name);
        script->source = source;
        script->path = QString(name).replace(" ", "_").toLower() + ".py";

        // unique file name, as the GUI editor does
        int n = 0;
        QString base = script->path.chopped(3);
        for (FixPyScript *s : fixPySettings->getScripts())
            if (s != script && s->path == script->path) script->path = QString("%1_%2.py").arg(base).arg(++n);
        script->changed = true;
        status = "installed";
    }
    fixPySettings->save();

    DataProcessor *dp = DataProcessorFactory::instance().getProcessor(name);
    if (!dp) return CommandResult::failure(Status::Internal, "processor was saved but not registered");

    if (request.args.contains("automation")) dp->setAutomation(automationFromName(request.args.value("automation").toString()));
    if (request.args.contains("automated-only")) dp->setAutomatedOnly(request.args.value("automated-only").toBool());

    QJsonObject data = processorJson(dp, false);
    data.insert("status", status);
    CommandResult result = CommandResult::success(data);
    result.text = QString("%1 Python processor '%2' (%3)\n").arg(status).arg(name).arg(data.value("file").toString());
    return result;
#else
    Q_UNUSED(env);
    Q_UNUSED(request);
    return CommandResult::failure(Status::Failed, "this GoldenCheetah was built without Python");
#endif
}

static CommandResult
removeProcessor(CommandEnvironment &env, const CommandRequest &request)
{
#ifdef GC_WANT_PYTHON
    CommandResult writable = sharedSettingsWritable(env);
    if (!writable.ok()) return writable;

    QString name = request.args.value("name").toString();
    loadPythonProcessors();
    if (!fixPySettings->getScript(name))
        return CommandResult::failure(Status::NotFound, QString("no Python processor called '%1'").arg(name));

    fixPySettings->deleteScript(name);
    QJsonObject data;
    data.insert("removed", name);
    return CommandResult::success(data);
#else
    Q_UNUSED(env);
    Q_UNUSED(request);
    return CommandResult::failure(Status::Failed, "this GoldenCheetah was built without Python");
#endif
}

static CommandResult
configureProcessor(CommandEnvironment &env, const CommandRequest &request)
{
    CommandResult writable = sharedSettingsWritable(env);
    if (!writable.ok()) return writable;

    QString name = request.args.value("name").toString();
    DataProcessor *dp = findProcessor(name);
    if (!dp) return CommandResult::failure(Status::NotFound, QString("no data processor called '%1'").arg(name));

    if (request.args.contains("automation")) dp->setAutomation(automationFromName(request.args.value("automation").toString()));
    if (request.args.contains("automated-only")) dp->setAutomatedOnly(request.args.value("automated-only").toBool());
    return CommandResult::success(processorJson(dp, false));
}

// run one processor on one activity, the ride file is modified in memory
static QString
runOn(DataProcessor *dp, RideItem *item, QString &output, bool &changed)
{
    changed = false;
    RideFile *ride = item->ride();
    if (!ride) return QString("can't open the activity file: %1").arg(item->errors().join("; "));

#ifdef GC_WANT_PYTHON
    if (!dp->isCoreProcessor() && fixPySettings) {
        FixPyScript *script = fixPySettings->getScript(dp->id());
        if (script) {
            // run the script as the processor would, but keep its output.
            // scripts don't say whether they changed anything, compare
            JsonFileReader json;
            QByteArray before = json.toByteArray(item->context, ride, true, true, true, true);
            FixPyRunner runner(item->context, ride, item, true);
            QString text;
            runner.run(script->source, script->iniKey, text);
            output = text.trimmed();
            if (runner.failed()) return QString("the script raised an error");
            changed = json.toByteArray(item->context, ride, true, true, true, true) != before;
            return QString();
        }
    }
#endif

    changed = dp->postProcess(ride, nullptr, "UPDATE");
    return QString();
}

static CommandResult
runProcessor(CommandEnvironment &env, const CommandRequest &request)
{
    QString name = request.args.value("name").toString();
    DataProcessor *dp = findProcessor(name);
    if (!dp) return CommandResult::failure(Status::NotFound, QString("no data processor called '%1'").arg(name));

#ifdef GC_WANT_PYTHON
    if (!dp->isCoreProcessor() && !HeadlessApp::pythonAvailable())
        return CommandResult::failure(Status::Failed, "Python is not available, can't run a Python processor");
#endif

    ActivitySelection selection = ActivitySelection::fromArgs(request.args);
    bool all = request.args.value("all").toBool(false);
    bool dryRun = request.args.value("dry-run").toBool(false);

    // don't rewrite every activity by accident
    if (selection.isEmpty() && !all)
        return CommandResult::failure(Status::Usage, "choose activities (by name, --filter, --from ...) or pass --all");

    QList<RideItem *> items;
    QString error;
    Status status;
    if (!selection.resolve(*env.session, items, error, status)) return CommandResult::failure(status, error);

    RideCache *cache = env.session->rideCache();
    QJsonArray report;
    QList<RideItem *> discard;
    int processed = 0, skipped = 0, failed = 0, saved = 0;
    QString text;

    for (RideItem *item : items) {
        env.report(QString("%1 %2").arg(dp->id()).arg(item->fileName));

        QJsonObject r;
        r.insert("activity", QFileInfo(item->fileName).completeBaseName());
        r.insert("start", activityStart(item));

        QString output;
        bool changed = false;
        QString why = runOn(dp, item, output, changed);
        if (!output.isEmpty()) r.insert("output", output);

        if (!why.isEmpty()) {
            r.insert("status", "failed");
            r.insert("message", why);
            failed++;
            // throw away a half done change
            item->close();
        } else if (!changed) {
            r.insert("status", "skipped");
            r.insert("message", "no change");
            skipped++;
        } else {
            // saved as we go, so a file that can't be written is reported as such
            item->setDirty(true);
            QString saveError;
            if (dryRun) {
                discard << item;
            } else if (cache->saveActivity(item, saveError)) {
                saved++;
            } else {
                r.insert("status", "failed");
                r.insert("message", saveError);
                failed++;
            }
            if (!r.contains("status")) {
                r.insert("status", "processed");
                processed++;
            }
        }
        report.append(r);

        text += QString("%1  %2").arg(r.value("status").toString(), -9).arg(r.value("activity").toString());
        if (r.contains("message")) text += "  " + r.value("message").toString();
        text += "\n";
        if (!output.isEmpty()) {
            for (const QString &line : output.split("\n")) text += "          | " + line + "\n";
        }
    }

    // the trends and CP estimates only use what is on disk
    if (saved) env.session->refresh();
    for (RideItem *item : discard) { item->setDirty(false); item->close(); }

    QJsonObject data;
    data.insert("processor", dp->id());
    data.insert("activities", report);
    data.insert("processed", processed);
    data.insert("skipped", skipped);
    data.insert("failed", failed);
    data.insert("saved", !dryRun);

    text += QString("%1 processed, %2 skipped, %3 failed%4\n").arg(processed).arg(skipped).arg(failed)
            .arg(dryRun ? " (dry run, nothing saved)" : "");

    CommandResult result = CommandResult::success(data);
    result.text = text;
    if (failed) {
        result.status = (processed || skipped) ? Status::Partial : Status::Failed;
        result.error = QString("%1 activit%2 failed").arg(failed).arg(failed == 1 ? "y" : "ies");
    }
    return result;
}

void
registerProcessorCommands(CommandRegistry &registry)
{
    Command list;
    list.spec.name = "processor.list";
    list.spec.summary = "list data processors, built in and Python";
    list.spec.scope = Scope::Global;
    list.spec.params << ParamSpec("type", ParamType::String, "only this kind").oneOf({ "builtin", "python" });
    list.spec.httpMethod = "GET";
    list.spec.httpPath = "/processors";
    list.handler = listProcessors;
    registry.add(list);

    Command show;
    show.spec.name = "processor.show";
    show.spec.summary = "describe a data processor (and show a Python processor's script)";
    show.spec.scope = Scope::Global;
    show.spec.params << ParamSpec("name", ParamType::String, "processor id or name").req().pos();
    show.spec.httpMethod = "GET";
    show.spec.httpPath = "/processors/{name}";
    show.handler = showProcessor;
    registry.add(show);

    Command install;
    install.spec.name = "processor.install";
    install.spec.summary = "install a Python data processor from a script file";
    install.spec.description =
        "Adds the script as a Python processor (Edit > Python Fixes in the GUI) under\n"
        "the given name, which later commands use to run it. Installing the same script\n"
        "again changes nothing; a different script needs --replace. Python processors\n"
        "are shared by all athletes in the athletes folder, as in the GUI.";
    install.spec.scope = Scope::Global;
    install.spec.modifies = true;
    install.spec.params << ParamSpec("name", ParamType::String, "name to install it under").req().pos();
    install.spec.params << ParamSpec("file", ParamType::Path, "python script file");
    install.spec.params << ParamSpec("source", ParamType::String, "the script itself, instead of --file");
    install.spec.params << ParamSpec("replace", ParamType::Bool, "replace an existing processor with the same name");
    install.spec.params << ParamSpec("automation", ParamType::String,
                                     "run automatically: manual (never), import (on import), save (on save)").oneOf(automationNames);
    install.spec.params << ParamSpec("automated-only", ParamType::Bool, "hide from the manual processor menus");
    install.spec.httpMethod = "PUT";
    install.spec.httpPath = "/processors/{name}";
    install.handler = installProcessor;
    registry.add(install);

    Command remove;
    remove.spec.name = "processor.remove";
    remove.spec.summary = "remove a Python data processor";
    remove.spec.scope = Scope::Global;
    remove.spec.modifies = true;
    remove.spec.params << ParamSpec("name", ParamType::String, "processor name").req().pos();
    remove.spec.httpMethod = "DELETE";
    remove.spec.httpPath = "/processors/{name}";
    remove.handler = removeProcessor;
    registry.add(remove);

    Command configure;
    configure.spec.name = "processor.configure";
    configure.spec.summary = "set when a data processor runs automatically";
    configure.spec.scope = Scope::Global;
    configure.spec.modifies = true;
    configure.spec.params << ParamSpec("name", ParamType::String, "processor id or name").req().pos();
    configure.spec.params << ParamSpec("automation", ParamType::String,
                                       "manual (never), import (on import), save (on save)").oneOf(automationNames);
    configure.spec.params << ParamSpec("automated-only", ParamType::Bool, "hide from the manual processor menus");
    configure.spec.httpMethod = "PATCH";
    configure.spec.httpPath = "/processors/{name}";
    configure.handler = configureProcessor;
    registry.add(configure);

    Command run;
    run.spec.name = "processor.run";
    run.spec.summary = "run a data processor on activities and save them";
    run.spec.description =
        "Runs the processor on each chosen activity and saves the ones it changed, so\n"
        "trends and critical power estimates use the result. Activities are chosen by\n"
        "name or the way the GUI filter does. Reports which activities were processed,\n"
        "skipped (the processor made no change) and failed.";
    run.spec.scope = Scope::Athlete;
    run.spec.modifies = true;
    run.spec.params << ParamSpec("name", ParamType::String, "processor id or name").req().pos();
    run.spec.params << ActivitySelection::params(true);
    run.spec.params << ParamSpec("all", ParamType::Bool, "run on every activity when nothing else is chosen");
    run.spec.params << ParamSpec("dry-run", ParamType::Bool, "run but don't save anything");
    run.spec.httpMethod = "POST";
    run.spec.httpPath = "/athletes/{athlete}/processors/{name}/runs";
    run.handler = runProcessor;
    registry.add(run);
}

} // namespace Headless
