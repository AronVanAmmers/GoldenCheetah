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
// Importing activity files, headless.
//
// The import itself is RideImporter, which the GUI import wizard
// (RideImportWizard) uses too, so an activity imported here is the same as
// one imported in the GUI: same file name, a copy of the original in
// /imports, the same metadata set on import, the automatic data processors
// run and planned activities linked. This file finds the files and reports.
//

#include "HeadlessCommands.h"
#include "ResultFormat.h"
#include "ActivitySelection.h"

#include "Context.h"
#include "Athlete.h"
#include "RideCache.h"
#include "RideItem.h"
#include "RideFile.h"
#include "RideMetadata.h"
#include "JsonRideFile.h"
#include "RideImporter.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QScopeGuard>
#include <exception>
#include <memory>

namespace Headless {

struct ImportOptions {
    bool dryRun = false;
    bool recursive = false;
};

struct ImportItem {
    QString source;         // what the user gave us (or file#n inside it)
    QString status;         // imported, skipped, failed, ok (dry run)
    QString activity;       // activity id (base name)
    QString start;          // local start time
    QString sport;
    QString message;        // why skipped / failed
    QStringList warnings;   // reader warnings

    QJsonObject json() const {
        QJsonObject o;
        o.insert("source", source);
        o.insert("status", status);
        if (!activity.isEmpty()) o.insert("activity", activity);
        if (!start.isEmpty()) o.insert("start", start);
        if (!sport.isEmpty()) o.insert("sport", sport);
        if (!message.isEmpty()) o.insert("message", message);
        if (!warnings.isEmpty()) o.insert("warnings", QJsonArray::fromStringList(warnings));
        return o;
    }
};

class Importer
{
    public:

        Importer(AthleteSession &session, const ImportOptions &options, const CommandEnvironment &env,
                 const QMap<QString, QString> &displayNames)
            : session(session), options(options), env(env), context(session.context()), displayNames(displayNames) {}

        ~Importer() {
            for (const QString &f : deleteMe) QFile::remove(f);
        }

        // turn the arguments into a list of files: folders are scanned,
        // archives are unpacked as the import window does
        QList<QPair<QString,QString>> expand(const QStringList &inputs, QList<ImportItem> &failures)
        {
            QList<QPair<QString,QString>> files; // label, path

            for (const QString &input : inputs) {
                QFileInfo info(input);

                if (!info.exists()) {
                    ImportItem item;
                    item.source = input;
                    item.status = "failed";
                    item.message = "file not found";
                    failures << item;
                    continue;
                }

                if (info.isDir()) {
                    QDirIterator::IteratorFlags flags = options.recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
                    QDirIterator it(info.absoluteFilePath(), QDir::Files | QDir::Readable, flags);
                    QStringList found;
                    while (it.hasNext()) {
                        QString f = it.next();
                        if (RideImporter::isImportable(f) || isArchive(f)) found << f;
                    }
                    found.sort();
                    for (const QString &f : found) expandFile(f, f, files);
                    continue;
                }

                // an upload is reported by the name the client gave it
                expandFile(displayNames.value(input, input), info.absoluteFilePath(), files);
            }
            return files;
        }

        ImportItem importOne(const QString &label, const QString &path)
        {
            ImportItem result;
            result.source = label;

            QFileInfo info(path);
            if (!info.isFile() || !info.isReadable()) {
                result.status = "failed";
                result.message = "not a readable file";
                return result;
            }

            // opendata files are not activities
            if (info.fileName().startsWith("{") && info.suffix().toLower() == "json") {
                result.status = "failed";
                result.message = "opendata files can't be imported";
                return result;
            }

            if (!RideImporter::isImportable(path)) {
                result.status = "failed";
                result.message = QString("unsupported file type '%1'").arg(RideImporter::activitySuffix(path).toLower());
                return result;
            }

            // parse it, some formats hold many activities
            QStringList errors;
            QList<RideFile*> rides;
            QFile file(path);
            std::unique_ptr<RideFile> ride(RideFileFactory::instance().openRideFile(context, file, errors, &rides));

            if (rides.count() > 1) {
                // as the wizard: write each one out as json and import those.
                // the returned ride may be one of the list, delete it only once
                if (rides.contains(ride.get())) ride.release();
                auto cleanup = qScopeGuard([&rides]() { qDeleteAll(rides); });
                for (RideFile *extracted : rides) extracted->context = context;
                QStringList written = RideImporter::splitActivities(context, path, rides,
                                                                    context->athlete->home->temp().absolutePath(), deleteMe);
                cleanup.dismiss();
                for (int i = 0; i < written.count(); i++)
                    pending << qMakePair(QString("%1#%2").arg(label).arg(i + 1), written.at(i));

                ImportItem summary;
                summary.source = label;
                summary.status = "expanded";
                summary.message = QString("%1 activities").arg(written.count());
                return summary;
            }

            if (!ride) {
                result.status = "failed";
                result.message = errors.isEmpty() ? QString("could not read the file") : errors.join("; ");
                return result;
            }

            // with a ride, errors are only warnings
            result.warnings = errors;

            QDateTime start = ride->startTime();
            if (!start.isValid()) {
                result.status = "failed";
                result.message = "the file has no start date and time";
                return result;
            }

            // local time to the second, as the wizard's date and time columns
            QDateTime when(start.date(), QTime(start.time().hour(), start.time().minute(), start.time().second()));
            QString target = RideImporter::targetName(when);
            QString activitiesTarget = target + ".json";
            result.start = when.toString("yyyy-MM-ddTHH:mm:ss");
            result.activity = target;

            // already imported?
            RideItem *existing = nullptr;
            switch (RideImporter::conflict(context, when, &existing)) {
            case RideImporter::Outcome::Exists:
                result.status = "skipped";
                result.message = QString("already imported as %1").arg(activitiesTarget);
                return result;
            case RideImporter::Outcome::SameStart:
                result.status = "skipped";
                result.message = QString("an activity with the same start time exists (%1)").arg(existing->fileName);
                result.activity = QFileInfo(existing->fileName).completeBaseName();
                return result;
            default:
                break;
            }

            if (options.dryRun) {
                result.status = "ok";
                result.message = "would be imported";
                return result;
            }

            // the rest exactly as the import window, with the ride already read
            RideImporter::Result saved = RideImporter::save(context, path, when, std::move(ride), errors, false,
                                                            [&](RideImporter::Step step, const RideImporter::Result &now) {
                if (step != RideImporter::Step::CopyFailed) return;
                QString copy = context->athlete->home->imports().absoluteFilePath(now.importsName);
                result.warnings << (QFile::exists(copy) ? QString("%1 is already in the imports folder, it was kept").arg(now.importsName)
                                                        : QString("could not copy the original to %1").arg(copy));
            });

            switch (saved.outcome) {
            case RideImporter::Outcome::Imported:
                result.status = "imported";
                if (saved.item) result.sport = saved.item->sport;
                break;
            case RideImporter::Outcome::WriteFailed:
                result.status = "failed";
                result.message = QString("could not write %1 to the tmpActivities folder").arg(activitiesTarget);
                break;
            case RideImporter::Outcome::MoveFailed:
                result.status = "failed";
                result.message = QString("could not move %1 to the activities folder").arg(activitiesTarget);
                break;
            default:
                result.status = "failed";
                result.message = "could not import the file";
                break;
            }
            return result;
        }

        QList<ImportItem> run(const QStringList &inputs)
        {
            QList<ImportItem> results;
            pending = expand(inputs, results);

            while (!pending.isEmpty()) {
                QPair<QString,QString> next = pending.takeFirst();
                env.report(QString("importing %1").arg(next.first));
                // a broken file must not take the whole import down
                try {
                    results << importOne(next.first, next.second);
                } catch (const std::exception &e) {
                    ImportItem failed;
                    failed.source = next.first;
                    failed.status = "failed";
                    failed.message = QString("the file could not be read (%1)").arg(e.what());
                    results << failed;
                }
            }

            // new activities get their metrics like any other change
            if (!options.dryRun) session.refresh();

            // report the sport as computed by the refresh
            ActivityLookup lookup(session.rideCache());
            for (ImportItem &r : results) {
                if (r.status != "imported" || !r.sport.isEmpty()) continue;
                QString why;
                RideItem *item = lookup.find(r.activity, why);
                if (item) r.sport = item->sport;
            }
            return results;
        }

    private:

        static bool isArchive(const QString &path) {
            QString s = QFileInfo(path).suffix().toLower();
            return s == "zip" || s == "gzip";
        }

        void expandFile(const QString &label, const QString &path, QList<QPair<QString,QString>> &files)
        {
            QStringList expanded = RideImporter::expand(context, QStringList() << path, deleteMe);
            if (expanded == QStringList() << path) {
                files << qMakePair(label, path);
                return;
            }
            for (const QString &e : expanded)
                files << qMakePair(QString("%1:%2").arg(label).arg(QFileInfo(e).fileName()), e);
        }

        AthleteSession &session;
        ImportOptions options;
        const CommandEnvironment &env;
        Context *context;
        QMap<QString, QString> displayNames;
        QStringList deleteMe;
        QList<QPair<QString,QString>> pending;
};

static CommandResult
importCommand(CommandEnvironment &env, const CommandRequest &request)
{
    QStringList files;
    for (const QJsonValue &v : request.args.value("file").toArray()) files << v.toString();

    ImportOptions options;
    options.dryRun = request.args.value("dry-run").toBool(false);
    options.recursive = request.args.value("recursive").toBool(false);

    Importer importer(*env.session, options, env, request.displayNames);
    QList<ImportItem> items = importer.run(files);

    QJsonArray list;
    int imported = 0, skipped = 0, failed = 0, ok = 0;
    QString text;
    for (const ImportItem &i : items) {
        if (i.status == "expanded") continue;
        list.append(i.json());
        if (i.status == "imported") imported++;
        else if (i.status == "skipped") skipped++;
        else if (i.status == "failed") failed++;
        else if (i.status == "ok") ok++;

        QString target;
        if (!i.activity.isEmpty()) target += "  -> " + i.activity;
        if (!i.sport.isEmpty()) target += " (" + i.sport + ")";
        text += ResultFormat::statusLine(i.json(), "source", 8, target);
        for (const QString &w : i.warnings) text += "          warning: " + w + "\n";
    }

    QJsonObject data;
    data.insert("files", list);
    data.insert("imported", imported);
    data.insert("skipped", skipped);
    data.insert("failed", failed);
    if (options.dryRun) data.insert("importable", ok);
    data.insert("dry_run", options.dryRun);

    if (options.dryRun) text += QString("%1 importable, %2 skipped, %3 failed (dry run)\n").arg(ok).arg(skipped).arg(failed);
    else text += QString("%1 imported, %2 skipped, %3 failed\n").arg(imported).arg(skipped).arg(failed);

    CommandResult result = CommandResult::batch(data, failed, imported + skipped + ok + failed,
                                                QString("%1 file(s) could not be imported").arg(failed));
    result.text = text;
    return result;
}

static CommandResult
formatsCommand(CommandEnvironment &, const CommandRequest &)
{
    const RideFileFactory &factory = RideFileFactory::instance();
    QJsonArray list;
    QStringList suffixes = factory.suffixes();
    suffixes.sort();
    QStringList writable = factory.writeSuffixes();
    for (const QString &s : suffixes) {
        QJsonObject o;
        o.insert("suffix", s);
        o.insert("description", factory.description(s));
        o.insert("import", true);
        o.insert("export", writable.contains(s));
        list.append(o);
    }
    QJsonObject data;
    data.insert("formats", list);
    return CommandResult::success(data);
}

void
registerImportCommands(CommandRegistry &registry)
{
    Command import;
    import.spec.name = "import";
    import.spec.summary = "import activity files, as the GUI import window";
    import.spec.description =
        "Imports .fit and every other format the GUI imports. Folders are scanned for\n"
        "activity files and archives are unpacked. Files whose activity is already in\n"
        "the athlete (same start time) are skipped. Imported activities are saved with\n"
        "the same samples, laps, sport and metadata the GUI import produces, and the\n"
        "data processors set to run automatically on import are run.";
    import.spec.scope = Scope::Athlete;
    import.spec.modifies = true;
    import.spec.params << ParamSpec("file", ParamType::Path, "activity file, archive or folder").req().pos().many().upload();
    import.spec.params << ParamSpec("recursive", ParamType::Bool, "scan folders recursively");
    import.spec.params << ParamSpec("dry-run", ParamType::Bool, "check what would be imported, change nothing");
    import.spec.httpMethod = "POST";
    import.spec.httpPath = "/athletes/{athlete}/imports";
    import.handler = importCommand;
    registry.add(import);

    Command formats;
    formats.spec.name = "formats";
    formats.spec.summary = "list the activity file formats that can be imported and exported";
    formats.spec.scope = Scope::Global;
    formats.spec.httpMethod = "GET";
    formats.spec.httpPath = "/formats";
    formats.handler = formatsCommand;
    registry.add(formats);
}

} // namespace Headless
