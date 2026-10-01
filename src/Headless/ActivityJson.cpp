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

#include "ActivityJson.h"

#include "IntervalData.h"

#include "RideItem.h"
#include "IntervalItem.h"
#include "RideMetric.h"

#include <QFileInfo>
#include <cmath>

namespace Headless {

QString
activityStart(RideItem *item)
{
    return item->dateTime.toLocalTime().toString("yyyy-MM-ddTHH:mm:ss");
}

QJsonValue
jsonNumber(double v)
{
    if (std::isnan(v) || std::isinf(v)) return QJsonValue();
    return QJsonValue(QString::number(v, 'g', 12).toDouble());
}

QJsonObject
activitySummary(RideItem *item)
{
    QJsonObject o;
    o.insert("id", QFileInfo(item->fileName).completeBaseName());
    o.insert("file", item->fileName);
    o.insert("start", activityStart(item));
    o.insert("sport", item->sport);
    if (item->planned) o.insert("planned", true);
    o.insert("duration", jsonNumber(item->getForSymbol("workout_time")));
    o.insert("distance", jsonNumber(item->getForSymbol("total_distance")));
    o.insert("data", item->present);
    return o;
}

void
addMetrics(QJsonObject &o, RideItem *item, const QStringList &symbols, bool metricUnits)
{
    if (symbols.isEmpty()) return;
    QJsonObject m;
    const RideMetricFactory &factory = RideMetricFactory::instance();
    for (const QString &symbol : symbols) {
        if (!factory.haveMetric(symbol)) continue;
        m.insert(symbol, jsonNumber(item->getForSymbol(symbol, metricUnits)));
    }
    o.insert("metrics", m);
}

void
addMetadata(QJsonObject &o, RideItem *item, const QStringList &fields)
{
    QJsonObject m;
    const QMap<QString,QString> &meta = item->metadata();
    if (fields.isEmpty()) {
        for (auto it = meta.constBegin(); it != meta.constEnd(); ++it) m.insert(it.key(), it.value());
    } else {
        for (const QString &f : fields) if (meta.contains(f)) m.insert(f, meta.value(f));
    }
    o.insert("metadata", m);
}

// as interval list and activity show give an interval
QJsonObject
intervalJson(IntervalItem *interval, int number)
{
    QJsonObject o;
    o.insert("number", number);
    o.insert("name", interval->name);
    o.insert("type", intervalTypeKey(interval->type));
    o.insert("group", RideFileInterval::typeDescription(interval->type));   // as the sidebar titles it
    o.insert("start", jsonNumber(interval->start));
    o.insert("stop", jsonNumber(interval->stop));
    o.insert("duration", jsonNumber(interval->stop - interval->start));
    o.insert("start_km", jsonNumber(interval->startKM));
    o.insert("stop_km", jsonNumber(interval->stopKM));
    o.insert("color", interval->color.name());
    if (interval->test) o.insert("test", true);
    return o;
}

} // namespace Headless
