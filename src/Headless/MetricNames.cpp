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

#include "MetricNames.h"

#include "RideMetric.h"

#include <QHash>

namespace Headless {

QString
metricFormulaName(const QString &symbol)
{
    const RideMetric *m = RideMetricFactory::instance().rideMetric(symbol);
    return m ? m->internalName().replace(" ", "_") : QString();
}

// symbols, formula names (as DataFilter looks them up) and display names,
// built again when the metrics change (user metrics added, renamed, removed)
static QHash<QString, QString> metricLookup;
static int lookupCount = -1;
static quint16 lookupSchema = 0;

void
invalidateMetricLookup()
{
    metricLookup.clear();
    lookupCount = -1;
}

QString
metricSymbol(const QString &name)
{
    const RideMetricFactory &factory = RideMetricFactory::instance();
    if (lookupCount != factory.metricCount() || lookupSchema != UserMetricSchemaVersion) {
        metricLookup.clear();
        lookupCount = factory.metricCount();
        lookupSchema = UserMetricSchemaVersion;

        // a compatibility_ metric only stands in for a user metric that went
        // away: when a name is taken by both, the real one wins
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < factory.metricCount(); i++) {
                QString symbol = factory.metricName(i);
                if (symbol.startsWith("compatibility_") != (pass == 0)) continue;
                const RideMetric *m = factory.rideMetric(symbol);
                if (!m) continue;
                metricLookup.insert(m->name().replace(" ", "_").toLower(), symbol);
                metricLookup.insert(metricFormulaName(symbol).toLower(), symbol);
            }
        }
        for (int i = 0; i < factory.metricCount(); i++) metricLookup.insert(factory.metricName(i).toLower(), factory.metricName(i));
    }
    if (factory.haveMetric(name)) return name;
    return metricLookup.value(QString(name).trimmed().replace(" ", "_").toLower());
}

bool
resolveMetrics(const QStringList &names, QStringList &symbols, QString &error)
{
    symbols.clear();
    for (const QString &n : names) {
        QString symbol = metricSymbol(n);
        if (symbol.isEmpty()) {
            error = QString("unknown metric '%1', see 'metric list'").arg(n);
            return false;
        }
        symbols << symbol;
    }
    return true;
}

} // namespace Headless
