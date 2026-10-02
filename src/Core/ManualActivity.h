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

#ifndef _GC_ManualActivity_h
#define _GC_ManualActivity_h 1

#include <QString>
#include <QDateTime>
#include <QList>

class Context;
class RideFile;
class ErgFile;
struct RideFilePoint;

//
// An activity entered by hand (Activity > Manual entry...) or planned
// (Plan activity...): what the ManualActivityWizard collects, how it turns
// that into an activity file, and its stress estimates. The wizard and the
// headless 'activity add' / 'plan add' commands both use it, so an activity
// entered either way is the same file.
//
// Values are as the wizard holds them once its fields are read: whole
// numbers where it has a spin box, distance in km and duration in seconds.
// Zero means "not given": nothing is stored for it.
//
struct ManualActivity
{
    QDateTime start;

    QString sport;              // as typed, e.g. "Bike"
    QString subSport;
    QString workoutCode;
    QString objective;          // plans only
    QString notes;
    int rpe = 0;                // 1-10, 0 = not given

    // a workout from the train library (plans only)
    QString workoutFilename;
    QString workoutTitle;       // stored as Route
    QString workoutDescription; // appended to the notes

    // the laps editor (Run and Swim), used instead of distance and duration
    bool paceIntervals = false;
    QList<RideFilePoint*> laps;

    double distance = 0;        // km
    int duration = 0;           // seconds
    int averageHr = 0;
    int averageCadence = 0;
    int averagePower = 0;
    int work = 0;               // kJ
    int bikeStress = 0;
    int bikeScore = 0;
    int swimScore = 0;
    int triScore = 0;
    int elevationGain = 0;
    int isoPower = 0;
    int xPower = 0;

    // the activity file the wizard writes: tags, metric overrides, laps
    // and the linked defaults
    void fill(RideFile &rideFile, bool plan) const;

    // write it as json in the activities (or planned) folder and add it to
    // the athlete. An existing file with that name is overwritten, as the
    // wizard does after warning about it. False when it can't be written.
    bool save(Context *context, bool plan) const;

    // choosing a workout from the train library (the wizard's workout page):
    // the workout's file, title and description, and what it implies (power,
    // stress and duration for erg workouts, distance for slope ones). ergFile
    // is the workout read for the activity's date, or null when it can't be.
    void setWorkout(const QString &filename, const QString &title, const QString &type,
                    const QString &description, int elevation, int durationMs, double distanceMeters,
                    const ErgFile *ergFile);

    // yyyy_MM_dd_HH_mm_ss, the activity's file name without the suffix
    static QString baseName(const QDateTime &start);

    // the path of the json file for that start
    static QString fileName(Context *context, const QDateTime &start, bool plan);

    // the wizard's stress estimates, from the athlete's activities of the
    // same sport in the last 'days' days (all of them when there are none).
    // sport is the standard name, as RideFile::sportTag gives it
    struct Estimate {
        double work = 0;        // kJ
        double bikeStress = 0;
        double bikeScore = 0;
        double swimScore = 0;
        double triScore = 0;
    };
    enum class EstimateBy { Duration, Distance };
    static Estimate estimate(Context *context, const QString &sport, int days, EstimateBy by,
                             double durationSeconds, double distanceKm);
};

#endif
