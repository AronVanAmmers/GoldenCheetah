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

#include "RideImporter.h"

#include "Context.h"
#include "Athlete.h"
#include "RideCache.h"
#include "RideItem.h"
#include "RideFile.h"
#include "RideMetadata.h"
#include "JsonRideFile.h"
#include "DataProcessor.h"
#include "ArchiveFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

QString
RideImporter::activitySuffix(const QString &path)
{
    // strip off gz or zip as openRideFile will sort that for us
    // since some file names contain "." as separator, not only for suffixes,
    // find the file-type suffix in a 2 step approach
    QStringList allNameParts = QFileInfo(path).fileName().split(".");
    QString suffix;
    if (!allNameParts.isEmpty()) {
        if (allNameParts.last().toLower() == "zip" ||
            allNameParts.last().toLower() == "gz") {
            // gz/zip are handled by openRideFile
            allNameParts.removeLast();
        }
        if (!allNameParts.isEmpty()) {
            suffix = allNameParts.last();
        }
    }
    return suffix;
}

bool
RideImporter::isImportable(const QString &path)
{
    QString suffix = activitySuffix(path);
    if (suffix.isEmpty()) return false;
    foreach (QString known, RideFileFactory::instance().suffixes())
        if (known.compare(suffix, Qt::CaseInsensitive) == 0) return true;
    return false;
}

QStringList
RideImporter::expand(Context *context, const QStringList &files, QStringList &deleteMe)
{
    // we keep a list of what we're returning
    QStringList expanded;
    static const QRegularExpression archives("^(zip|gzip)$", QRegularExpression::CaseInsensitiveOption);

    foreach(QString file, files) {

        if (archives.match(QFileInfo(file).suffix()).hasMatch()) {
            // its an archive so lets check - but only to one depth
            // archives that contain archives can get in the sea
            QList<QString> contents = Archive::dir(file);
            if (contents.count() == 0) expanded << file;
            else {
                // we need to extract the contents and return those
                QStringList ex = Archive::extract(file, contents, const_cast<AthleteDirectoryStructure*>(context->athlete->directoryStructure())->tmpActivities().absolutePath());
                deleteMe += ex;
                expanded += ex;
            }

        } else {
            expanded << file;
        }
    }

    return expanded;
}

QStringList
RideImporter::splitActivities(Context *context, const QString &source, QList<RideFile *> &rides,
                              const QString &folder, QStringList &deleteMe)
{
    // we write as JSON to ensure we don't lose data e.g. XDATA.
    QStringList written;
    int counter = 0;
    foreach(RideFile *extracted, rides) {

        // write as a temporary file, using the original
        // filename with "-n" appended
        QString fulltarget = folder + "/" + QFileInfo(source).baseName() + QString("-%1.json").arg(counter+1);
        JsonFileReader reader;
        QFile target(fulltarget);
        reader.writeRideFile(context, extracted, target);
        deleteMe.append(fulltarget);
        delete extracted;
        written << fulltarget;
        counter++;
    }
    rides.clear();
    return written;
}

bool
RideImporter::moveFile(const QString &source, const QString &target)
{
    QFile r(source);

    // first try it with a rename
    if (r.rename(target)) return true; // job is done

    // now the harder variant (copy & delete)
    if (r.copy(target))
      {
        // try to remove - but if this fails, no problem, file has been copied at least
        r.remove();
        // even if remove failed, the copy was successful - so GC is fine
        return true;
      }

    // more required ?

    return false;
}

QString
RideImporter::targetName(const QDateTime &ridedatetime)
{
    QChar zero = QLatin1Char ( '0' );
    return QString ( "%1_%2_%3_%4_%5_%6" )
            .arg ( ridedatetime.date().year(), 4, 10, zero )
            .arg ( ridedatetime.date().month(), 2, 10, zero )
            .arg ( ridedatetime.date().day(), 2, 10, zero )
            .arg ( ridedatetime.time().hour(), 2, 10, zero )
            .arg ( ridedatetime.time().minute(), 2, 10, zero )
            .arg ( ridedatetime.time().second(), 2, 10, zero );
}

RideImporter::Outcome
RideImporter::conflict(Context *context, const QDateTime &ridedatetime, RideItem **existing)
{
    // check if a ride at this point of time already exists in /activities - if yes, skip import
    QString finalActivitiesFulltarget = context->athlete->home->activities().canonicalPath() + "/" + targetName(ridedatetime) + ".json";
    if (QFileInfo(finalActivitiesFulltarget).exists()) return Outcome::Exists;

    // in addition, also check the RideCache for a Ride with the same point in Time in UTC, which also indicates
    // that there was already a ride imported - reason is that RideCache start time is in UTC, while the file Name is in "localTime"
    // which causes problems when importing the same file (for files which do not have time/date in the file name),
    // while the computer has been set to a different time zone
    RideItem *item = context->athlete->rideCache->getRide(ridedatetime.toUTC());
    if (existing) *existing = item;
    return item ? Outcome::SameStart : Outcome::Imported;
}

RideImporter::Result
RideImporter::save(Context *context, const QString &path, const QDateTime &ridedatetime,
                   std::unique_ptr<RideFile> parsed, const QStringList &parsedErrors, bool signal,
                   const std::function<void(Step, const Result &)> &step)
{
    Result result;
    AthleteDirectoryStructure *home = const_cast<AthleteDirectoryStructure*>(context->athlete->directoryStructure());
    QDir homeImports = home->imports();
    QDir homeActivities = home->activities();
    QDir tmpActivities = home->tmpActivities();

    // prepare the new file names for the next steps - basic name and .JSON in GC format
    QString targetnosuffix = targetName(ridedatetime);
    QString activitiesTarget = QString ("%1.%2" ).arg ( targetnosuffix ).arg ( "json" );
    result.activity = activitiesTarget;

    // create filenames incl. directory path for GC .JSON for both /tmpActivities and /activities directory
    QString tmpActivitiesFulltarget = tmpActivities.canonicalPath() + "/" + activitiesTarget;
    QString finalActivitiesFulltarget = homeActivities.canonicalPath() + "/" + activitiesTarget;

    // a ride at this point of time already exists - skip import
    Outcome taken = conflict(context, ridedatetime);
    if (taken != Outcome::Imported) { result.outcome = taken; return result; }

    // copy the sourceFile to /imports ONLY if the source is NOT coming from /imports itself
    // add the date/time of the target to the source file name (for identification)
    QFileInfo sourceFileInfo (path);
    QString importsTarget;
    if (sourceFileInfo.canonicalPath() != homeImports.canonicalPath()) {

        // add the GC file base name to create unique file names during import
        // there should not be 2 ride files with exactly the same time stamp (as this is also not foreseen for the .json)
        importsTarget = sourceFileInfo.baseName() + "_" + targetnosuffix + "." + sourceFileInfo.suffix();
        QString importsFulltarget = homeImports.canonicalPath() + "/" + importsTarget;
        // copy the source file to /imports with adjusted name
        result.importsName = importsTarget;
        QFile source(path);
        if (!source.copy(importsFulltarget)) {
            if (step) step(Step::CopyFailed, result);
        }
    } else {
        // file is re-imported from /imports - keep the name for .JSON Source File Tag
        importsTarget = sourceFileInfo.fileName();
        result.importsName = importsTarget;
    }

    // open the file with the respective format reader and export as .JSON
    // to track if addRideCache() has caused an error due to bad data we work with a interim directory for the activities
    // -- first   export to /tmpactivities
    // -- second  create RideCache() entry
    // -- third   move file from /tmpactivities to /activities
    std::unique_ptr<RideFile> ride = std::move(parsed);
    if (ride) result.errors = parsedErrors;
    else {
        QFile thisfile(path);
        ride.reset(RideFileFactory::instance().openRideFile(context, thisfile, result.errors));
    }

    // did the input file parse ok ? (should be fine here - since it was alrady checked before - but just in case)
    if (!ride) { result.outcome = Outcome::ReadFailed; return result; }

    // update ridedatetime and set the Source File name
    ride->setStartTime(ridedatetime);
    ride->setTag("Source Filename", importsTarget);
    ride->setTag("Filename", activitiesTarget);
    if (result.errors.count() > 0)
        ride->setTag("Import errors", result.errors.join("\n"));

    // process linked defaults
    GlobalContext::context()->rideMetadata->setLinkedDefaults(ride.get());

    // run the processor first... import
    if (step) step(Step::Processing, result);
    DataProcessorFactory::instance().autoProcess(ride.get(), "Auto", "Import");
    ride->recalculateDerivedSeries();
    // now metrics have been calculated
    DataProcessorFactory::instance().autoProcess(ride.get(), "Save", "ADD");

    if (step) step(Step::Saving, result);

    // serialize
    JsonFileReader reader;
    QFile target(tmpActivitiesFulltarget);
    bool written = reader.writeRideFile(context, ride.get(), target);
    ride.reset();
    if (!written) { result.outcome = Outcome::WriteFailed; return result; }

    // now try adding the Ride to the RideCache - since this may fail due to various reason, the activity file
    // is stored in tmpActivities during this process to understand which file has create the problem when restarting GC
    // - only after the step was successful the file is moved
    // to the "clean" activities folder
    context->athlete->addRide(QFileInfo(tmpActivitiesFulltarget).fileName(), signal,
                              true, true);  // file is available only in /tmpActivities, so use this one please

    // rideCache is successfully updated, let's move the file to the real /activities
    if (!moveFile(tmpActivitiesFulltarget, finalActivitiesFulltarget)) { result.outcome = Outcome::MoveFailed; return result; }

    // and correct the path locally stored in Ride Item (addRide selected it)
    result.item = context->ride;
    if (result.item) {
        result.item->setFileName(homeActivities.canonicalPath(), activitiesTarget);

        // try autolinking to planned activity
        RideItem *other = context->athlete->rideCache->findSuggestion(result.item);
        RideCache::OperationPreCheck check = context->athlete->rideCache->checkLinkActivities(result.item, other);
        if (check.canProceed && ! check.requiresUserDecision) {
            RideCache::OperationResult linked = context->athlete->rideCache->linkActivities(result.item, other);
            if (linked.success) {
                QString error;
                context->athlete->rideCache->saveActivities(check.affectedItems, error);
            }
        }
    }
    result.outcome = Outcome::Imported;
    return result;
}
