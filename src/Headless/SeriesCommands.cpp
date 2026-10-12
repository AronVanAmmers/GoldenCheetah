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

//
// An activity's data series: the samples read out, and one series written,
// from values worked out elsewhere or from a formula. A write is applied as
// a data processor applies its edits (one undoable change made of point
// edits) and saved.
//

#include "HeadlessCommands.h"
#include "ResultFormat.h"
#include "ActivityJson.h"

#include "Context.h"
#include "RideCache.h"
#include "RideItem.h"
#include "RideFile.h"
#include "RideFileCommand.h"
#include "DataFilter.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>
#include <memory>

namespace Headless {

// the series an activity file stores: these can be read and written
static const RideFile::SeriesType storedSeries[] = {
    RideFile::secs, RideFile::km, RideFile::watts, RideFile::nm, RideFile::cad, RideFile::kph, RideFile::hr,
    RideFile::alt, RideFile::lat, RideFile::lon, RideFile::headwind, RideFile::slope, RideFile::temp,
    RideFile::lrbalance, RideFile::lte, RideFile::rte, RideFile::lps, RideFile::rps, RideFile::lpco,
    RideFile::rpco, RideFile::lppb, RideFile::rppb, RideFile::lppe, RideFile::rppe, RideFile::lpppb,
    RideFile::rpppb, RideFile::lpppe, RideFile::rpppe, RideFile::smo2, RideFile::thb, RideFile::rvert,
    RideFile::rcad, RideFile::rcontact,
};

// by the names formulas use: POWER, SPEED, SECS ...
static QStringList
storedSeriesNames()
{
    QStringList names;
    for (RideFile::SeriesType type : storedSeries) names << RideFile::symbolForSeries(type);
    return names;
}

static bool
storedSeriesType(const QString &name, RideFile::SeriesType &type, QString &error)
{
    for (RideFile::SeriesType t : storedSeries) {
        if (RideFile::symbolForSeries(t).compare(name, Qt::CaseInsensitive)) continue;
        type = t;
        return true;
    }
    error = QString("unknown series '%1', the series are: %2").arg(name, storedSeriesNames().join(", "));
    return false;
}

// a formula as typed in the GUI, checked
static bool
parseFormula(Context *context, const QString &option, const QString &text, std::unique_ptr<DataFilter> &df, QString &error)
{
    df.reset(new DataFilter(nullptr, context, text));
    if (df->getErrors().isEmpty()) return true;
    error = QString("%1: %2").arg(option, df->getErrors().join("; "));
    return false;
}

// where a sample is, for messages
static QString
sampleAt(const RideFilePoint *p)
{
    return QString("SECS = %1").arg(QString::number(p->secs, 'g', 12));
}

static CommandResult
showSeries(CommandEnvironment &env, const CommandRequest &request)
{
    QList<RideFile::SeriesType> types { RideFile::secs };
    QStringList names { "SECS" };
    QString error;
    for (const QJsonValue &v : request.args.value("series").toArray()) {
        RideFile::SeriesType type;
        if (!storedSeriesType(v.toString(), type, error)) return CommandResult::failure(Status::Usage, error);
        if (types.contains(type)) continue;
        types << type;
        names << RideFile::symbolForSeries(type);
    }
    QString whereText = request.args.value("where").toString().trimmed();
    std::unique_ptr<DataFilter> where;
    if (!whereText.isEmpty() && !parseFormula(env.session->context(), "--where", whereText, where, error))
        return CommandResult::failure(Status::Usage, error);

    RideItem *item = env.session->findActivity(request.args.value("activity").toString(), error);
    if (!item) return CommandResult::failure(Status::NotFound, error);
    bool wasOpen = item->isOpen();
    RideFile *ride = item->ride();
    if (!ride) return CommandResult::failure(Status::Failed, QString("can't open the activity file: %1").arg(item->errors().join("; ")));
    for (int k = 1; k < types.count(); k++) {
        if (ride->isDataPresent(types.at(k))) continue;
        if (!wasOpen) item->close();
        return CommandResult::failure(Status::Failed, QString("the activity has no %1 samples").arg(names.at(k)));
    }

    QJsonArray rows;
    for (RideFilePoint *p : ride->dataPoints()) {
        if (where && !where->evaluate(item, p).number()) continue;
        QJsonObject row;
        for (int k = 0; k < types.count(); k++) row.insert(names.at(k), jsonNumber(p->value(types.at(k))));
        rows.append(row);
    }
    if (!wasOpen) item->close();

    QJsonObject data;
    data.insert("activity", QFileInfo(item->fileName).completeBaseName());
    data.insert("samples", rows);
    CommandResult result = CommandResult::success(data);
    result.text = ResultFormat::table(rows, names);
    result.csv = ResultFormat::csv(data, names);
    return result;
}

// SECS and SERIES from a file 'activity series show' wrote, as CSV or JSON,
// possibly edited elsewhere
static bool
readValues(const QString &path, const QString &series, QVector<double> &secs, QVector<double> &values, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QString("can't read %1").arg(path);
        return false;
    }
    QString text = QString::fromUtf8(file.readAll()).trimmed();
    if (text.startsWith(QChar(0xFEFF))) text = text.mid(1).trimmed();

    // a number in JSON or in a cell
    auto number = [&](const QJsonValue &v, const QString &cell, int row, const QString &column, double &out) {
        bool ok = false;
        if (v.isDouble()) {
            out = v.toDouble();
            ok = true;
        } else if (!cell.isNull()) out = cell.toDouble(&ok);
        if (ok && std::isfinite(out)) return true;
        error = QString("row %1 of %2: %3 is not a number").arg(row).arg(path, column);
        return false;
    };

    if (text.startsWith('[') || text.startsWith('{')) {
        QJsonParseError parsed;
        QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parsed);
        if (doc.isNull()) {
            error = QString("%1 is not valid JSON: %2").arg(path, parsed.errorString());
            return false;
        }
        // the rows, the data of 'activity series show', or its whole result
        QJsonObject o = doc.object();
        if (o.contains("data")) o = o.value("data").toObject();
        QJsonArray rows = doc.isArray() ? doc.array() : o.value("samples").toArray();
        for (int r = 0; r < rows.count(); r++) {
            QJsonValue s, v;
            QJsonObject row = rows.at(r).toObject();
            for (auto it = row.constBegin(); it != row.constEnd(); ++it) {
                if (!it.key().compare("SECS", Qt::CaseInsensitive)) s = it.value();
                if (!it.key().compare(series, Qt::CaseInsensitive)) v = it.value();
            }
            double a, b;
            if (!number(s, QString(), r + 1, "SECS", a) || !number(v, QString(), r + 1, series, b)) return false;
            secs << a;
            values << b;
        }
    } else {
        // a header, then numbers; other columns are ignored
        QStringList lines = text.split(QRegularExpression("\r?\n"), Qt::SkipEmptyParts);
        auto cells = [](const QString &line) {
            QStringList out;
            for (QString c : line.split(',')) {
                c = c.trimmed();
                if (c.length() >= 2 && c.startsWith('"') && c.endsWith('"')) c = c.mid(1, c.length() - 2).trimmed();
                out << c;
            }
            return out;
        };
        QStringList header = cells(lines.value(0));
        int s = -1, v = -1;
        for (int c = 0; c < header.count(); c++) {
            if (!header.at(c).compare("SECS", Qt::CaseInsensitive)) s = c;
            if (!header.at(c).compare(series, Qt::CaseInsensitive)) v = c;
        }
        if (s < 0 || v < 0) {
            error = QString("%1 needs a header with the columns SECS and %2").arg(path, series);
            return false;
        }
        for (int r = 1; r < lines.count(); r++) {
            QStringList row = cells(lines.at(r));
            double a, b;
            if (!number(QJsonValue(), row.value(s, ""), r, "SECS", a) || !number(QJsonValue(), row.value(v, ""), r, series, b))
                return false;
            secs << a;
            values << b;
        }
    }
    if (secs.isEmpty()) {
        error = QString("%1 has no values").arg(path);
        return false;
    }
    return true;
}

static CommandResult
setSeries(CommandEnvironment &env, const CommandRequest &request)
{
    QString error;
    RideFile::SeriesType target;
    if (!storedSeriesType(request.args.value("series").toString(), target, error))
        return CommandResult::failure(Status::Usage, error);
    QString name = RideFile::symbolForSeries(target);
    if (target == RideFile::secs)
        return CommandResult::failure(Status::Usage, "SECS is when each sample was recorded, it can't be set");

    // values from a file, or a formula on the samples --where picks
    QString whereText = request.args.value("where").toString().trimmed();
    QString exprText = request.args.value("expression").toString();
    QString valuesPath = request.args.value("values").toString();
    bool dryRun = request.args.value("dry-run").toBool(false);
    if (valuesPath.isEmpty() == exprText.isEmpty())
        return CommandResult::failure(Status::Usage, "give either --values or --expression");
    if (!valuesPath.isEmpty() && !whereText.isEmpty())
        return CommandResult::failure(Status::Usage, "--values says which samples to write, --where is for --expression");

    std::unique_ptr<DataFilter> where, expr;
    Context *context = env.session->context();
    if (!whereText.isEmpty() && !parseFormula(context, "--where", whereText, where, error))
        return CommandResult::failure(Status::Usage, error);
    if (!exprText.isEmpty() && !parseFormula(context, "--expression", exprText, expr, error))
        return CommandResult::failure(Status::Usage, error);
    QVector<double> fileSecs, fileValues;
    if (!valuesPath.isEmpty() && !readValues(valuesPath, name, fileSecs, fileValues, error))
        return CommandResult::failure(Status::Usage, error);

    RideItem *item = env.session->findActivity(request.args.value("activity").toString(), error);
    if (!item) return CommandResult::failure(Status::NotFound, error);
    QString id = QFileInfo(item->fileName).completeBaseName();

    // an activity opened here is closed again when done; closing it also
    // throws away an edit that couldn't be saved
    bool wasOpen = item->isOpen();
    auto failed = [&](const QString &why) {
        if (!wasOpen) item->close();
        return CommandResult::failure(Status::Failed, QString("%1: %2").arg(id, why));
    };

    RideFile *ride = item->ride();
    if (!ride) return failed(QString("can't open the activity file: %1").arg(item->errors().join("; ")));
    const QVector<RideFilePoint *> &points = ride->dataPoints();
    if (points.isEmpty()) return failed("the activity has no samples");

    // the samples to write and their new values, all worked out on the
    // samples as they are before anything is written
    QVector<int> rows;
    QVector<double> after;
    if (expr) {
        for (int i = 0; i < points.count(); i++)
            if (!where || where->evaluate(item, points.at(i)).number()) rows << i;
        if (rows.isEmpty()) return failed(QString("no sample has %1, nothing was written").arg(whereText));

        // once for the activity: a number, or one value per sample
        Result r = expr->evaluate(item, nullptr);
        if (!r.isNumber)
            return failed("--expression gives text, not numbers; a series name such as POWER is a sample's value only "
                          "in --where, use samples(POWER) for the series");
        QVector<double> vector = r.asNumeric();
        if (!vector.isEmpty() && vector.count() != points.count())
            return failed(QString("--expression gives %1 values for %2 samples, nothing was written")
                          .arg(vector.count()).arg(points.count()));
        for (int i : rows) {
            double v = vector.isEmpty() ? r.number() : vector.at(i);
            if (!std::isfinite(v))
                return failed(QString("--expression is %1 at %2, nothing was written").arg(QString::number(v), sampleAt(points.at(i))));
            after << v;
        }
    } else {
        // each value goes to the sample at its time
        QHash<int, int> given;
        for (int k = 0; k < fileSecs.count(); k++) {
            double s = fileSecs.at(k);
            int i = ride->timeIndex(s);
            if (i > 0 && std::fabs(points.at(i - 1)->secs - s) < std::fabs(points.at(i)->secs - s)) i--;
            if (i < 0 || std::fabs(points.at(i)->secs - s) > 1e-6)
                return failed(QString("no sample has SECS = %1, nothing was written").arg(QString::number(s, 'g', 12)));
            if (given.contains(i))
                return failed(QString("SECS = %1 is given twice, nothing was written").arg(QString::number(s, 'g', 12)));
            given.insert(i, k);
        }
        for (int i = 0; i < points.count(); i++) {
            if (!given.contains(i)) continue;
            rows << i;
            after << fileValues.at(given.value(i));
        }
    }

    bool present = ride->isDataPresent(target);
    QVector<double> before;
    int changed = 0;
    for (int k = 0; k < rows.count(); k++) {
        double old = present ? points.at(rows.at(k))->value(target) : 0;
        if (!present || after.at(k) != old) changed++;
        before << old;
    }
    auto mean = [](const QVector<double> &v) {
        double sum = 0;
        for (double x : v) sum += x;
        return jsonNumber(v.isEmpty() ? 0 : sum / v.count());
    };

    QJsonObject data;
    data.insert("activity", id);
    data.insert("series", name);
    if (expr) {
        data.insert("where", whereText);
        data.insert("expression", exprText);
    } else {
        data.insert("values", request.displayNames.value(valuesPath, valuesPath));
    }
    data.insert("samples", points.count());
    data.insert("written", rows.count());
    data.insert("changed", changed);
    data.insert("from_secs", jsonNumber(points.at(rows.first())->secs));
    data.insert("to_secs", jsonNumber(points.at(rows.last())->secs));
    if (present) data.insert("mean_before", mean(before));
    data.insert("mean_after", mean(after));

    QString status;
    if (changed == 0) status = "unchanged";
    else if (dryRun) status = "dry run";
    else {
        // as a data processor edits a ride: one undoable change of point edits,
        // which the save records in the activity's Change History
        QString how = expr ? QString("where %1").arg(whereText.isEmpty() ? "every sample" : whereText)
                           : QString("from %1").arg(QFileInfo(data.value("values").toString()).fileName());
        ride->command->startLUW(QString("Set %1 %2").arg(name, how));
        if (!present) ride->command->setDataPresent(target, true);
        for (int k = 0; k < rows.count(); k++) ride->command->setPointValue(rows.at(k), target, after.at(k));
        ride->command->endLUW();

        item->setDirty(true);
        QString saveError;
        if (!env.session->rideCache()->saveActivity(item, saveError)) {
            item->setDirty(false);
            item->close();
            return CommandResult::failure(Status::Failed, QString("%1: %2").arg(id, saveError));
        }
        status = "updated";
    }
    data.insert("status", status);
    data.insert("saved", status == "updated");
    if (!wasOpen) item->close();

    // metrics, trends and CP estimates follow the file on disk
    if (status == "updated") env.session->refresh();

    CommandResult result = CommandResult::success(data);
    result.text = QString("%1  %2  %3 on %4 of %5 samples (%6 changed)%7\n")
                  .arg(status, id, name).arg(rows.count()).arg(points.count()).arg(changed)
                  .arg(dryRun && changed ? ", dry run, nothing saved" : "");
    result.text += QString("  %1 on the samples written: mean %2 -> %3\n").arg(name)
                   .arg(present ? QString::number(data.value("mean_before").toDouble()) : QString("(none)"))
                   .arg(data.value("mean_after").toDouble());
    return result;
}

void
registerSeriesCommands(CommandRegistry &registry)
{
    Command show;
    show.spec.name = "activity.series.show";
    show.spec.summary = "an activity's samples: SECS and the series asked for";
    show.spec.description =
        "One row per sample, by the names formulas use (POWER, SPEED, HEARTRATE ...).\n"
        "With --format csv or json it is the file 'activity series set --values' reads.";
    show.spec.scope = Scope::Athlete;
    show.spec.params << ParamSpec("activity", ParamType::String, "activity id, start time, date, 'first' or 'last'").req().pos();
    show.spec.params << ParamSpec("series", ParamType::String, "series, e.g. POWER SPEED").req().pos().many();
    show.spec.params << ParamSpec("where", ParamType::String, "only the samples where this formula holds, e.g. 'SECS < 135'");
    show.spec.httpMethod = "GET";
    show.spec.httpPath = "/athletes/{athlete}/activities/{activity}/series";
    show.handler = showSeries;
    registry.add(show);

    Command set;
    set.spec.name = "activity.series.set";
    set.spec.summary = "set one data series of an activity, from values or a formula, and save it";
    set.spec.description =
        "--values FILE writes the values of a file shaped as 'activity series show'\n"
        "writes it (CSV or JSON, with SECS and SERIES), each to the sample at that SECS.\n"
        "Samples not in the file are left as they are.\n"
        "\n"
        "--expression writes a formula in the GUI's formula language, worked out once\n"
        "for the activity: a number, or one value per sample, such as samples(SPEED)\n"
        "times something. --where picks the samples it is written to (every sample\n"
        "without it) and is worked out at each sample, where SECS, POWER, SPEED ...\n"
        "are that sample's values. In formulas x / 0 is 0 and the mean of nothing is 0,\n"
        "so check with --dry-run first.\n"
        "\n"
        "Other series, XData, intervals and fields are left as they are. The edit is\n"
        "one change saved as a data processor saves its edits, then metrics are\n"
        "recomputed. Nothing is written when a value isn't a finite number or has no\n"
        "sample to go to.";
    set.spec.scope = Scope::Athlete;
    set.spec.modifies = true;
    set.spec.params << ParamSpec("activity", ParamType::String, "activity id, start time, date, 'first' or 'last'").req().pos();
    set.spec.params << ParamSpec("series", ParamType::String, "the series to write, e.g. POWER").req().pos();
    set.spec.params << ParamSpec("values", ParamType::Path, "CSV or JSON file with SECS and the series, as 'activity series show' writes it").upload();
    set.spec.params << ParamSpec("expression", ParamType::String, "formula for the new values, e.g. 'round(samples(SPEED) * 30)'");
    set.spec.params << ParamSpec("where", ParamType::String, "with --expression, the samples to write, e.g. 'SECS < 135' (default: every sample)");
    set.spec.params << ParamSpec("dry-run", ParamType::Bool, "report what would be written, save nothing");
    set.spec.httpMethod = "PATCH";
    set.spec.httpPath = "/athletes/{athlete}/activities/{activity}/series/{series}";
    set.handler = setSeries;
    registry.add(set);
}

} // namespace Headless
