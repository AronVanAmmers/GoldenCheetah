/*
 * Copyright (c) 2009 Eric Murray (ericm@lne.com)
 * Copyright (c) 2014 Mark Liversedge (liversedge@gmail.com)
 * Copyright (c) 2025 Joachim Kohlhammer (joachim.kohlhammer@gmx.de)
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

#include "ManualActivity.h"

#include "Context.h"
#include "Athlete.h"
#include "RideCache.h"
#include "RideItem.h"
#include "RideFile.h"
#include "RideMetadata.h"
#include "Settings.h"
#include "Units.h"
#include "ErgFile.h"

#include <QFile>
#include <QMap>

static void
setTagString(RideFile &rideFile, const QString &tagName, const QString &value)
{
    if (! value.trimmed().isEmpty()) {
        rideFile.setTag(tagName, value.trimmed());
    }
}


static void
setTagInt(RideFile &rideFile, const QString &tagName, int value)
{
    if (value > 0) {
        rideFile.setTag(tagName, QString::number(value));
    }
}


static void
setMetricDouble(RideFile &rideFile, const QString &metricName, double value)
{
    if (value > 0) {
        QMap<QString,QString> values;
        values.insert("value", QString::number(value));
        rideFile.metricOverrides.insert(metricName, values);
    }
}


static void
setMetricInt(RideFile &rideFile, const QString &metricName, int value)
{
    if (value > 0) {
        QMap<QString,QString> values;
        values.insert("value", QString::number(value));
        rideFile.metricOverrides.insert(metricName, values);
    }
}


void
ManualActivity::fill
(RideFile &rideFile, bool plan) const
{
    QString sportTag = RideFile::sportTag(sport.trimmed());

    rideFile.setStartTime(start);
    rideFile.setRecIntSecs(0.00);
    rideFile.setDeviceType("Manual");
    rideFile.setFileFormat("GoldenCheetah Json");
    if (plan) {
        rideFile.setTag("Original Date", start.date().toString("yyyy/MM/dd"));
    }

    setTagString(rideFile, "Sport", sport);
    setTagString(rideFile, "SubSport", subSport);
    setTagString(rideFile, "Workout Code", workoutCode);
    setTagInt(rideFile, "RPE", rpe);
    setTagString(rideFile, "Objective", objective);
    setTagString(rideFile, "WorkoutFilename", workoutFilename);
    setTagString(rideFile, "Route", workoutTitle);

    // Special case notes: Combine notes and workout description (if available)
    QString notesCombined = notes.trimmed();
    QString description = workoutDescription.trimmed();
    if (! description.isEmpty()) {
        if (! notesCombined.isEmpty()) {
            notesCombined += "\n";
        }
        notesCombined += description;
    }
    rideFile.setTag("Notes", notesCombined);

    if ((sportTag == "Run" || sportTag == "Swim") && paceIntervals) {
        // get samples from Laps Editor, if available
        if (laps.count() > 0) {
            rideFile.setRecIntSecs(1.00);
            for (RideFilePoint *point : laps) {
                rideFile.appendPoint(*point);
            }
            rideFile.fillInIntervals();
        }
    } else {
        setMetricDouble(rideFile, "total_distance", distance);
        setMetricInt(rideFile, "workout_time", duration);
        setMetricInt(rideFile, "time_riding", duration);
    }
    setMetricInt(rideFile, "average_hr", averageHr);
    setMetricInt(rideFile, "average_cad", averageCadence);
    setMetricInt(rideFile, "average_power", averagePower);
    setMetricInt(rideFile, "total_work", work);
    setMetricInt(rideFile, "coggan_tss", bikeStress);
    setMetricInt(rideFile, "skiba_bike_score", bikeScore);
    setMetricInt(rideFile, "swimscore", swimScore);
    setMetricInt(rideFile, "triscore", triScore);
    setMetricInt(rideFile, "elevation_gain", elevationGain);
    setMetricInt(rideFile, "coggan_np", isoPower);
    setMetricInt(rideFile, "skiba_xpower", xPower);

    // process linked defaults
    GlobalContext::context()->rideMetadata->setLinkedDefaults(&rideFile);
}


bool
ManualActivity::save
(Context *context, bool plan) const
{
    RideFile rideFile;
    fill(rideFile, plan);

    QFile out(fileName(context, start, plan));
    if (! RideFileFactory::instance().writeRideFile(context, &rideFile, out, "json")) {
        return false;
    }

    // refresh metric db etc
    context->athlete->addRide(baseName(start) + ".json", true, true, false, plan);
    return true;
}


void
ManualActivity::setWorkout
(const QString &filename, const QString &title, const QString &type, const QString &description,
 int elevation, int durationMs, double distanceMeters, const ErgFile *ergFile)
{
    workoutFilename = filename;
    workoutTitle = title;
    workoutDescription = description;
    elevationGain = elevation;

    averagePower = 0;
    bikeStress = 0;
    bikeScore = 0;
    isoPower = 0;
    xPower = 0;
    if (ergFile != nullptr && type == "erg") {
        averagePower = static_cast<int>(ergFile->AP());
        bikeStress = static_cast<int>(ergFile->bikeStress());
        bikeScore = static_cast<int>(ergFile->BS());
        isoPower = static_cast<int>(ergFile->IsoPower());
        xPower = static_cast<int>(ergFile->XP());
    }

    duration = 0;
    distance = 0;
    if (type == "erg") {
        duration = durationMs / 1000;
    } else if (type == "slp") {
        distance = distanceMeters / 1000;
    }
}


QString
ManualActivity::baseName
(const QDateTime &dt)
{
    return dt.toString("yyyy_MM_dd_HH_mm_ss");
}


QString
ManualActivity::fileName
(Context *context, const QDateTime &dt, bool plan)
{
    QString basename = baseName(dt);
    QString filename;
    if (plan) {
        filename = context->athlete->home->planned().canonicalPath() + "/" + basename + ".json";
    } else {
        filename = context->athlete->home->activities().canonicalPath() + "/" + basename + ".json";
    }
    return filename;
}


ManualActivity::Estimate
ManualActivity::estimate
(Context *context, const QString &sport, int estimationDays, EstimateBy estimateBy,
 double actDuration, double actDistance)
{

    double timeWork = 0.0;
    double distanceWork = 0.0;
    double timeBikeStress = 0.0;
    double distanceBikeStress = 0.0;
    double timeBikeScore = 0.0;
    double distanceBikeScore = 0.0;
    double timeSwimScore = 0.0;
    double distanceSwimScore = 0.0;
    double timeTriScore = 0.0;
    double distanceTriScore = 0.0;

    double metricFactor = 1.0;
    if (   (sport == "Run" && ! appsettings->value(nullptr, GC_PACE, GlobalContext::context()->useMetricUnits).toBool())
        || (sport == "Swim" && ! appsettings->value(nullptr, GC_SWIMPACE, GlobalContext::context()->useMetricUnits).toBool())
        || (sport != "Run" && sport != "Swim" && ! GlobalContext::context()->useMetricUnits)) {
        metricFactor = MILES_PER_KM;
    }

    // do we have any rides?
    if (context->athlete->rideCache->rides().count()) {
        // last 'n' days calculation
        double seconds = 0.0;
        double distance = 0.0;
        double work = 0.0;
        double bikeStress = 0.0;
        double bikeScore = 0.0;
        double swimScore = 0.0;
        double triScore = 0.0;
        int rides = 0;

        // fall back to 'all time' calculation
        double totalSeconds = 0.0;
        double totalDistance = 0.0;
        double totalWork = 0.0;
        double totalBikeStress = 0.0;
        double totalBikeScore = 0.0;
        double totalSwimScore = 0.0;
        double totalTriScore = 0.0;

        // iterate over the ride cache
        for (RideItem *ride : context->athlete->rideCache->rides()) {
            if (ride->planned || ride->sport.trimmed() != sport) {
                continue;
            }

            // skip those with no time or distance values (not comparing doubles)
            if (ride->getForSymbol("time_riding") == 0 || ride->getForSymbol("total_distance") == 0) {
                continue;
            }

            // how many days ago was it?
            int daysAgo = ride->dateTime.daysTo(QDateTime::currentDateTime());

            // only use rides in last 'n' days
            if (daysAgo >= 0 && daysAgo < estimationDays) {
                seconds += ride->getForSymbol("time_riding");
                distance += ride->getForSymbol("total_distance");
                work += ride->getForSymbol("total_work");
                bikeStress += ride->getForSymbol("coggan_tss");
                bikeScore += ride->getForSymbol("skiba_bike_score");
                swimScore += ride->getForSymbol("swimscore");
                triScore += ride->getForSymbol("triscore");

                rides++;
            }
            totalSeconds += ride->getForSymbol("time_riding");
            totalDistance += ride->getForSymbol("total_distance");
            totalWork += ride->getForSymbol("total_work");
            totalBikeStress += ride->getForSymbol("coggan_tss");
            totalBikeScore += ride->getForSymbol("skiba_bike_score");
            totalSwimScore += ride->getForSymbol("swimscore");
            totalTriScore += ride->getForSymbol("triscore");
        }

        // total values, not just last 'n' days -- but avoid divide by zero
        totalDistance *= metricFactor;

        timeWork = (totalWork * 3600) / totalSeconds;
        timeBikeStress = (totalBikeStress * 3600) / totalSeconds;
        timeBikeScore = (totalBikeScore * 3600) / totalSeconds;
        timeSwimScore = (totalSwimScore * 3600) / totalSeconds;
        timeTriScore = (totalTriScore * 3600) / totalSeconds;
        distanceWork = totalWork / totalDistance;
        distanceBikeStress = totalBikeStress / totalDistance;
        distanceBikeScore = totalBikeScore / totalDistance;
        distanceSwimScore = totalSwimScore / totalDistance;
        distanceTriScore = totalTriScore / totalDistance;

        // don't use defaults if we have rides in last 'n' days
        if (rides) {
            if (seconds) {
                distance *= metricFactor;
                timeWork = (work * 3600) / seconds;
                timeBikeStress = (bikeStress * 3600) / seconds;
                timeBikeScore = (bikeScore * 3600) / seconds;
                timeSwimScore = (swimScore * 3600) / seconds;
                timeTriScore = (triScore * 3600) / seconds;
            }
            if (distance) {
                distanceWork = work / distance;
                distanceBikeStress = bikeStress / distance;
                distanceBikeScore = bikeScore / distance;
                distanceSwimScore = swimScore / distance;
                distanceTriScore = triScore / distance;
            }
        }
    }

    Estimate e;
    if (estimateBy == EstimateBy::Duration) {
        e.work = actDuration * timeWork / 3600.0;
        e.bikeStress = actDuration * timeBikeStress / 3600.0;
        e.bikeScore = actDuration * timeBikeScore / 3600.0;
        e.swimScore = actDuration * timeSwimScore / 3600.0;
        e.triScore = actDuration * timeTriScore / 3600.0;
    } else {
        e.work = actDistance * distanceWork;
        e.bikeStress = actDistance * distanceBikeStress;
        e.bikeScore = actDistance * distanceBikeScore;
        e.swimScore = actDistance * distanceSwimScore;
        e.triScore = actDistance * distanceTriScore;
    }
    return e;
}
