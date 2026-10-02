/*
 * Copyright (c) 2026 Joachim Kohlhammer (joachim.kohlhammer@gmx.de)
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


#ifndef PLANBUNDLE_H
#define PLANBUNDLE_H

#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QList>

#include "RideFile.h"
#include "RideItem.h"
#include "RideCache.h"
#include "Context.h"
#include "Season.h"


struct PlanExportDescription {
    QString name;
    QString author;
    QString sport;
    QString description;
    QString copyright;
    bool preferOriginal = true;
    QDate rangeStart;
    QDate rangeEnd;
    QStringList activityFiles;
    QString planFile;

    // the description with $NAME, $AUTHOR, $SPORT and $COPYRIGHT filled in
    QString expandedDescription() const;
};


// a planned activity in a source period, for repeating or exporting a plan
struct SourceRide {
    RideItem *rideItem = nullptr;
    QDate sourceDate;
    QDate targetDate;
    bool selected = false;
    int conflictGroup = -1;
    bool targetBlocked = false;
};


class PlanMetadata {
public:
    static const QString ManifestFilename;
    static const QString ReadmeFilename;

    int bundleVersion = -1;
    QString name;
    QString author;
    QString sport;
    QString description;
    QString copyright;
    QDateTime exportedAt;
    int durationDays = 0;
    int frontGapDays = 0;
    int backGapDays = 0;

    void reset();
    bool isValid() const;
    QList<QString> save(const QDir &dir) const;
    bool load(const QDir &dir);

private:
    QJsonObject toJson() const;
    bool fromJson(const QJsonObject &obj);
};


struct PlanResult {
    QStringList errors;
    QStringList warnings;

    bool ok() const;
    void addError(const QString &error);
    void addWarning(const QString &warning);
    void reset();
};


class RideFileSelection {
public:
    RideFileSelection(RideFile *rideFile, bool sel, const QDateTime &dt);

    RideFile *getRideFile() const;

    bool selected = false;
    QDateTime targetDateTime;

private:
    RideFile *rideFile = nullptr;
};


class PlanBundleReader {
    Q_DISABLE_COPY(PlanBundleReader)

public:
    explicit PlanBundleReader(Context *context, const QDate &targetDate);
    virtual ~PlanBundleReader();

    PlanMetadata getMetadata() const;
    PlanResult loadBundle(const QString &bundlePath);
    bool isValid() const;
    bool isNull() const;
    PlanResult importBundle();
    QDate getTargetRangeStart() const;
    QDate getTargetRangeEnd() const;
    int getDuration() const;
    int getNumActivities() const;
    int getNumSelectedActivities() const;
    const QStringList &getActivitiesToRemove() const;
    QList<RideItem*> getRideItemsToRemove() const;
    bool isIncludeGapDays() const;
    void setIncludeGapDays(bool includeGapDays);
    const QSet<QDateTime> &getExistingLinked() const;

    PlanResult getLastValidationResult() const;
    PlanResult getLastImportResult() const;

    QList<RideFileSelection> rideFiles;

private:
    Context *context;
    PlanMetadata metadata;
    QTemporaryDir *tempDir = nullptr;
    PlanResult lastValidationResult;
    PlanResult lastImportResult;
    QDate targetRangeStart;
    QDate targetRangeEnd;
    int duration = 0;
    int daysToAdd = 0;
    bool includeGapDays = true;
    QStringList toDelete;
    QSet<QDateTime> existingLinked;
    QDir plannedDir;
    QDir workoutDir;
    QHash<QString, QString> trainDBHashes;

    void reset();
    void calculateRange();
    void findConflicts();
    bool cleanAndCopyActivity(RideFile *rideFile) const;
    bool processWorkout(RideFile *rideFile);
    void validate();
};


namespace PlanBundle {
    QString getRideName(RideItem const * const rideItem);
    QString getRideName(RideFile const * const rideFile);
    QString getRideSport(RideItem const * const rideItem);
    QString getRideSport(RideFile const * const rideFile);
    QDate getRideDate(RideItem const * const rideItem, bool preferOriginal);
    QDate getRideDate(RideFile const * const rideFile, bool preferOriginal);

    bool exportBundle(Context *context, const PlanExportDescription &description);

    // the planned activities dated (originally, with preferOriginal) from
    // start to end, by date. Activities that would start on the same day at
    // the same time are a conflict group, of which only the first is selected
    QList<SourceRide> sourceRides(Context *context, const QDate &start, const QDate &end, bool preferOriginal);

    // a plan name as a file name: no spaces or characters files can't have
    QString sanitizeFilename(QString input);
}


//
// Repeating a plan (the Repeat Plan wizard): the planned activities of a
// source period are copied to the period starting on the target date. The
// unlinked planned activities already in the target period are deleted,
// and a copy that would start when a linked one does is left out.
//
class RepeatPlan {
public:
    RepeatPlan(Context *context, const QDate &targetStart);

    QList<SourceRide> sourceRides;

    // choose the source period; recollects the source rides when it changed
    void update(const QDate &sourceStart, const QDate &sourceEnd, bool keepGap, bool preferOriginal);

    // recompute the target period after the selection changed
    void update();

    QDate getTargetRangeStart() const;
    QDate getTargetRangeEnd() const;
    const QList<RideItem*> &getDeletionList() const;

    // the copies that will be made: selected and not blocked
    QList<std::pair<RideItem*, QDate>> copies() const;

    // delete the deletion list and make the copies. check tells whether the
    // copies could be made at all, result how making them went
    void apply(RideCache::OperationPreCheck &check, RideCache::OperationResult &result);

private:
    Context *context;
    QDate sourceRangeStart;
    QDate sourceRangeEnd;
    QDate targetRangeStart;
    QDate targetRangeEnd;
    int frontGap = 0;
    QList<RideItem*> deletionList;
    bool keepGap = false;
    bool preferOriginal = false;
};

#endif
