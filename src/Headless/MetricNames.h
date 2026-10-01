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

#ifndef _GC_MetricNames_h
#define _GC_MetricNames_h 1

#include <QString>
#include <QStringList>

namespace Headless {

//
// Metrics by the names people use: the symbol (average_power), the name
// formulas and the GUI's tables use (Average_Power, W'_Work) or the
// display name, any case.
//

// a metric by symbol (average_power) or by the name formulas and the GUI's
// tables use (Average_Power, W'_Work), any case; empty when unknown
QString metricSymbol(const QString &name);

// the lookup behind metricSymbol is rebuilt when the metric count or the
// user metrics change; this forces it, for when a user metric is replaced
void invalidateMetricLookup();
QString metricFormulaName(const QString &symbol);

// symbols for names, false and error for the first unknown one
bool resolveMetrics(const QStringList &names, QStringList &symbols, QString &error);


} // namespace Headless

#endif
