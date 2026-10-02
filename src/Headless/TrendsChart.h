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

#ifndef _GC_Headless_TrendsChart_h
#define _GC_Headless_TrendsChart_h 1

//
// Drawing the Trends sidebar charts (config/charts.xml) the way the Trends
// view draws them, with the GUI's own LTMPlot, and their data table.
//

#include "CommandRegistry.h"
#include "LTMSettings.h"

#include <QList>
#include <QString>

namespace Headless {

// chart library render and chart library data
void registerTrendsChartCommands(CommandRegistry &registry);

// the chart called name, or -1 with error set (ChartLibraryCommands.cpp)
int findChart(const QList<LTMSettings> &charts, const QString &name, QString &error);

} // namespace Headless

#endif
