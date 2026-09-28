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

class PDModel;
class PMCData;
class Context;
class RideItem;

namespace Headless {

class AthleteSession;

// the activities chosen by the selection parameters in args
bool selectedFiles(AthleteSession &session, const QJsonObject &args, QList<RideItem*> &items,
                   QString &error, Status &status);

// mean maximal values for the chosen activities (index = seconds)
QVector<double> meanMax(AthleteSession &session, const QJsonObject &args, RideFile::SeriesType series,
                        QString &error, Status &status, int &count);

// fit a power-duration model by short name (cp2, cp3, extended, multi, ws), caller owns it
PDModel *fitModel(Context *context, const QString &name, const QVector<double> &data);
QStringList modelNames();

RideFile::SeriesType seriesFromName(const QString &name, bool &ok);
QStringList seriesNames();

// performance manager data, owned by the athlete
PMCData *pmcFor(AthleteSession &session, const QString &metric, int sts, int lts);

} // namespace Headless

#endif
