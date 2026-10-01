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

#ifndef _GC_RideImporter_h
#define _GC_RideImporter_h 1

#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QList>
#include <functional>
#include <memory>

class Context;
class RideFile;
class RideItem;

//
// The steps of importing an activity file that don't need a window, shared
// by the import wizard (RideImportWizard) and the command line import, so
// both give the same result: the same file name, a copy of the original in
// /imports, the same metadata, the automatic data processors run, and a
// planned activity linked.
//
class RideImporter
{
    public:

        // the file type suffix, ignoring .gz/.zip compression (openRideFile
        // decompresses those)
        static QString activitySuffix(const QString &path);
        static bool isImportable(const QString &path);

        // archives (.zip, .gzip) are unpacked into the athlete's
        // tmpActivities folder, one level deep; the extracted files go into
        // deleteMe. Anything else is returned as it is.
        static QStringList expand(Context *context, const QStringList &files, QStringList &deleteMe);

        // a file of several activities, parsed into rides: each one is
        // written as a JSON file in folder (named after the source, -1, -2 ...)
        // to be imported in its place. The rides are deleted.
        static QStringList splitActivities(Context *context, const QString &source, QList<RideFile *> &rides,
                                           const QString &folder, QStringList &deleteMe);

        // rename, or else copy and delete
        static bool moveFile(const QString &source, const QString &target);

        // the activity's file name for a start time, without the suffix
        static QString targetName(const QDateTime &when);

        enum class Outcome {
            Imported,       // saved in /activities and added
            Exists,         // /activities has a file for that start time
            SameStart,      // an activity starts at the same time (in UTC)
            ReadFailed,     // the file didn't open as an activity
            WriteFailed,    // the activity couldn't be written to /tmpActivities
            MoveFailed      // or moved from there to /activities
        };

        // the save step reports where it is, for a progress display
        enum class Step { CopyFailed, Processing, Saving };

        struct Result {
            Outcome outcome = Outcome::ReadFailed;
            QString activity;           // file name in /activities, yyyy_MM_dd_HH_mm_ss.json
            QString importsName;        // the original's name in /imports
            QStringList errors;         // the reader's (warnings, as the ride was read)
            RideItem *item = nullptr;   // the activity added (Imported)
        };

        // is there an activity for this start time (local) already? Exists,
        // SameStart (with existing set), or Imported when there is none
        static Outcome conflict(Context *context, const QDateTime &when, RideItem **existing = nullptr);

        // import one activity file starting at when, local time: copy the
        // original to /imports, set the metadata, run the import and save
        // processors, write it to /tmpActivities, add it to the ride cache,
        // move it to /activities and link a planned activity. parsed is the
        // file already read with the reader's errors, or nullptr to read it
        // now. signal is passed to Athlete::addRide.
        static Result save(Context *context, const QString &path, const QDateTime &when,
                           std::unique_ptr<RideFile> parsed = nullptr, const QStringList &parsedErrors = QStringList(),
                           bool signal = false, const std::function<void(Step, const Result &)> &step = nullptr);
};

#endif
