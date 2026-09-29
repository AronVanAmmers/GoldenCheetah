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

#include "ResultFormat.h"

#include <QTextStream>
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>

namespace Headless {

QJsonObject
ResultFormat::envelope(const QString &command, const CommandResult &result)
{
    QJsonObject o;
    o.insert("ok", result.status == Status::Ok);
    o.insert("status", statusName(result.status));
    o.insert("command", command);
    o.insert("data", result.data);
    if (!result.warnings.isEmpty()) o.insert("warnings", QJsonArray::fromStringList(result.warnings));
    if (!result.error.isEmpty()) o.insert("error", result.error);
    return o;
}

QString
ResultFormat::scalar(const QJsonValue &v)
{
    switch (v.type()) {
    case QJsonValue::Null: return "-";
    case QJsonValue::Undefined: return "";
    case QJsonValue::Bool: return v.toBool() ? "yes" : "no";
    case QJsonValue::Double: {
        double d = v.toDouble();
        if (std::isnan(d)) return "-";
        if (d == std::floor(d) && std::fabs(d) < 1e15) return QString::number(qint64(d));
        // enough precision to be useful, not so much it's noise
        return QString::number(d, 'f', std::fabs(d) >= 100 ? 1 : 3).remove(QRegularExpression("\\.?0+$"));
    }
    case QJsonValue::String: return v.toString();
    case QJsonValue::Array: {
        QStringList parts;
        for (const QJsonValue &x : v.toArray()) parts << scalar(x);
        return parts.join(", ");
    }
    case QJsonValue::Object: {
        QStringList parts;
        QJsonObject o = v.toObject();
        for (const QString &k : o.keys()) parts << k + "=" + scalar(o.value(k));
        return parts.join(" ");
    }
    }
    return QString();
}

// identifying columns come first in tables, in this order
static const QStringList preferred = { "number", "id", "name", "type", "activity", "start", "stop", "duration", "date", "status", "sport", "symbol" };

// flatten one level of nesting so metrics.x become columns
static QJsonObject
flatten(const QJsonObject &row)
{
    QJsonObject out;
    for (const QString &k : row.keys()) {
        QJsonValue v = row.value(k);
        if (v.isObject()) {
            QJsonObject inner = v.toObject();
            for (const QString &ik : inner.keys()) out.insert(ik, inner.value(ik));
        } else {
            out.insert(k, v);
        }
    }
    return out;
}

QString
ResultFormat::table(const QJsonArray &rows)
{
    if (rows.isEmpty()) return "(none)\n";

    // columns in order of first appearance, keeping a stable, useful order
    // for the common ones
    QStringList columns;
    QList<QJsonObject> flat;
    for (const QJsonValue &r : rows) {
        QJsonObject o = r.isObject() ? flatten(r.toObject()) : QJsonObject{ { "value", r } };
        flat << o;
    }

    for (const QString &p : preferred)
        for (const QJsonObject &o : flat) if (o.contains(p) && !columns.contains(p)) { columns << p; break; }
    for (const QJsonObject &o : flat)
        for (const QString &k : o.keys()) if (!columns.contains(k)) columns << k;

    // long free text makes tables unreadable, it belongs in show/json
    static const QStringList skip = { "description", "source", "output" };
    for (const QString &s : skip) if (columns.count() > 2) columns.removeAll(s);

    QVector<int> widths(columns.count());
    QList<QStringList> cells;
    for (int c = 0; c < columns.count(); c++) widths[c] = columns[c].length();
    for (const QJsonObject &o : flat) {
        QStringList line;
        for (int c = 0; c < columns.count(); c++) {
            QString s = o.contains(columns[c]) ? scalar(o.value(columns[c])) : QString();
            s.replace('\n', ' ');
            if (s.length() > 60) s = s.left(57) + "...";
            widths[c] = std::max(widths[c], int(s.length()));
            line << s;
        }
        cells << line;
    }

    QString text;
    QTextStream out(&text);
    for (int c = 0; c < columns.count(); c++)
        out << (c ? "  " : "") << (c == columns.count() - 1 ? columns[c] : columns[c].leftJustified(widths[c]));
    out << "\n";
    for (const QStringList &line : cells) {
        for (int c = 0; c < columns.count(); c++) {
            QString cell = (c == columns.count() - 1) ? line[c] : line[c].leftJustified(widths[c]);
            out << (c ? "  " : "") << cell;
        }
        out << "\n";
    }
    return text;
}

static void
renderInto(QTextStream &out, const QJsonObject &data, int indent)
{
    QString pad(indent, ' ');

    // scalars first, then nested structures
    int width = 0;
    for (const QString &k : data.keys()) {
        QJsonValue v = data.value(k);
        if (!v.isObject() && !(v.isArray() && !v.toArray().isEmpty() && v.toArray().first().isObject()))
            width = std::max(width, int(k.length()));
    }
    for (const QString &k : data.keys()) {
        QJsonValue v = data.value(k);
        bool tableArray = v.isArray() && !v.toArray().isEmpty() && v.toArray().first().isObject();
        if (v.isObject() || tableArray) continue;
        out << pad << (k + ":").leftJustified(width + 2) << ResultFormat::scalar(v) << "\n";
    }
    for (const QString &k : data.keys()) {
        QJsonValue v = data.value(k);
        if (v.isObject()) {
            out << pad << k << ":\n";
            renderInto(out, v.toObject(), indent + 2);
        } else if (v.isArray() && !v.toArray().isEmpty() && v.toArray().first().isObject()) {
            out << pad << k << ":\n";
            QString t = ResultFormat::table(v.toArray());
            for (const QString &line : t.split('\n', Qt::SkipEmptyParts)) out << pad << "  " << line << "\n";
        }
    }
}

QString
ResultFormat::render(const QJsonObject &data)
{
    // a single list is shown as just the table
    QStringList keys = data.keys();
    QString listKey;
    int lists = 0;
    for (const QString &k : keys) {
        QJsonValue v = data.value(k);
        if (v.isArray() && (v.toArray().isEmpty() || v.toArray().first().isObject())) { listKey = k; lists++; }
    }
    if (lists == 1) {
        QString text = table(data.value(listKey).toArray());
        QJsonObject rest = data;
        rest.remove(listKey);
        QString extra;
        QTextStream out(&extra);
        renderInto(out, rest, 0);
        return text + (extra.isEmpty() ? QString() : "\n" + extra);
    }

    QString text;
    QTextStream out(&text);
    renderInto(out, data, 0);
    return text;
}

//
// CSV
//

QString
ResultFormat::csvValue(const QJsonValue &v)
{
    switch (v.type()) {
    case QJsonValue::Bool: return v.toBool() ? "true" : "false";
    case QJsonValue::Double: {
        double d = v.toDouble();
        if (std::isnan(d) || std::isinf(d)) return QString();
        if (d == std::floor(d) && std::fabs(d) < 1e15) return QString::number(qint64(d));
        return QString::number(d, 'g', 12);
    }
    case QJsonValue::String: return v.toString();
    case QJsonValue::Array: {
        QStringList parts;
        for (const QJsonValue &x : v.toArray()) parts << csvValue(x);
        return parts.join("; ");
    }
    case QJsonValue::Object: return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    default: return QString();
    }
}

QString
ResultFormat::csvLine(const QStringList &fields)
{
    QStringList out;
    for (QString f : fields) {
        if (f.contains(',') || f.contains('"') || f.contains('\n') || f.contains('\r')
            || f.startsWith(' ') || f.endsWith(' ')) {
            f.replace("\"", "\"\"");
            f = "\"" + f + "\"";
        }
        out << f;
    }
    return out.join(",") + "\n";
}

// nested objects become columns named by their keys, as in the text table,
// prefixed with the object's name where that would clash
static QJsonObject
csvFlatten(const QJsonObject &row)
{
    QJsonObject out;
    for (const QString &k : row.keys()) if (!row.value(k).isObject()) out.insert(k, row.value(k));
    for (const QString &k : row.keys()) {
        if (!row.value(k).isObject()) continue;
        QJsonObject inner = row.value(k).toObject();
        for (const QString &ik : inner.keys()) {
            QString name = out.contains(ik) ? k + "." + ik : ik;
            out.insert(name, inner.value(ik));
        }
    }
    return out;
}

// key,value lines for everything, arrays of objects by index
static void
csvPaths(QStringList &lines, const QString &path, const QJsonValue &v)
{
    if (v.isObject()) {
        QJsonObject o = v.toObject();
        for (const QString &k : o.keys()) csvPaths(lines, path.isEmpty() ? k : path + "." + k, o.value(k));
    } else if (v.isArray() && !v.toArray().isEmpty() && (v.toArray().first().isObject() || v.toArray().first().isArray())) {
        QJsonArray a = v.toArray();
        for (int i = 0; i < a.count(); i++) csvPaths(lines, QString("%1[%2]").arg(path).arg(i), a.at(i));
    } else {
        lines << ResultFormat::csvLine({ path, ResultFormat::csvValue(v) });
    }
}

QString
ResultFormat::csv(const QJsonObject &data)
{
    // a list is the table when everything else is a plain value (a count,
    // the activity), so nothing is lost by leaving those out
    QString listKey;
    int lists = 0, structured = 0;
    for (const QString &k : data.keys()) {
        QJsonValue v = data.value(k);
        if (v.isArray() && (v.toArray().isEmpty() || v.toArray().first().isObject())) { listKey = k; lists++; }
        if (v.isObject() || v.isArray()) structured++;
    }

    if (lists == 1 && structured == 1) {
        QList<QJsonObject> flat;
        for (const QJsonValue &r : data.value(listKey).toArray()) flat << csvFlatten(r.toObject());

        // the text table's column order
            QStringList columns;
        for (const QString &p : preferred)
            for (const QJsonObject &o : flat) if (o.contains(p) && !columns.contains(p)) { columns << p; break; }
        for (const QJsonObject &o : flat)
            for (const QString &k : o.keys()) if (!columns.contains(k)) columns << k;

        QString text = csvLine(columns);
        for (const QJsonObject &o : flat) {
            QStringList fields;
            for (const QString &c : columns) fields << csvValue(o.value(c));
            text += csvLine(fields);
        }
        return text;
    }

    QStringList lines;
    lines << csvLine({ "key", "value" });
    csvPaths(lines, QString(), data);
    return lines.join(QString());
}

QString
ResultFormat::csv(const CommandResult &result)
{
    if (!result.csv.isEmpty()) return result.csv;
    return csv(result.data);
}

QString
ResultFormat::text(const CommandResult &result)
{
    if (!result.text.isEmpty()) return result.text;
    return render(result.data);
}

} // namespace Headless
