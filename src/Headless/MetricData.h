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

#ifndef _GC_MetricData_h
#define _GC_MetricData_h 1

// data shared by the metric commands and the charts drawn from them

#include "HeadlessCommand.h"
#include "RideFile.h"

#include <QVector>
#include <QDate>
#include <memory>

class PDModel;
class PMCData;
class Context;
class RideItem;

namespace Headless {

class AthleteSession;

// the activities chosen by the selection parameters in args
bool selectedFiles(AthleteSession &session, const QJsonObject &args, QList<RideItem*> &items,
                   QString &error, Status &status);

// selection args with the sport defaulted to Bike for power-duration work
QJsonObject powerSelection(const QJsonObject &args);

// mean maximal values for the chosen activities (index = seconds)
QVector<double> meanMax(AthleteSession &session, const QJsonObject &args, RideFile::SeriesType series,
                        QString &error, Status &status, int &count);

// fit a power-duration model by short name (cp2, cp3, extended, multi, ws), caller owns it
PDModel *fitModel(Context *context, const QString &name, const QVector<double> &data);
QStringList modelNames();

RideFile::SeriesType seriesFromName(const QString &name, bool &ok);
QStringList seriesNames();

// performance manager data with the athlete's time constants, owned (and
// cached by metric) by the athlete
PMCData *pmcFor(AthleteSession &session, const QString &metric);

// the PMC for a command's arguments: the athlete's own, or with --sts or
// --lts, --sport or --filter one made here (into owned), filtered the way
// LTMPlot filters a PMC curve: the activities that pass, planned ones
// included, over all dates
PMCData *pmcForArgs(AthleteSession &session, const QJsonObject &args, const QString &metric,
                    std::unique_ptr<PMCData> &owned, QString &error, Status &status);

// the series --series takes: actual, planned and expected, and all
QStringList pmcSeriesNames(bool withAll);

// one day of a series of a PMC
struct PMCDay {
    double stress = 0, lts = 0, sts = 0, sb = 0, rr = 0;
};
PMCDay pmcDay(PMCData *pmc, const QString &series, const QDate &date);

// the last day a planned activity passes for this PMC, or an invalid date
QDate lastPlannedDay(AthleteSession &session, const QJsonObject &args);

// the parameters that choose the PMC: --series, --sport, --filter, --season
QList<ParamSpec> pmcParams(bool withAll);

} // namespace Headless

#endif
