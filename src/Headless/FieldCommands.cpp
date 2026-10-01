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
// Activity (metadata) field definitions, as edited in
// Options > Data Fields. GoldenCheetah keeps them in metadata.xml in the
// athletes folder, so they are shared by every athlete in that folder.
//

#include "HeadlessCommands.h"
#include "ResultFormat.h"

#include "Context.h"
#include "RideMetadata.h"

#include <QDir>
#include <QFile>

extern QString gcroot;

namespace Headless {

static const QStringList typeNames = {
    "text", "textbox", "shorttext", "int", "double", "date", "time", "checkbox"
};

static QString
typeName(GcFieldType type)
{
    int i = static_cast<int>(type);
    if (i >= 0 && i < typeNames.count()) return typeNames.at(i);
    return "none";
}

static GcFieldType
typeFromName(const QString &name)
{
    int i = typeNames.indexOf(name.toLower());
    if (i < 0) return GcFieldType::NO_FIELD_SET;
    return static_cast<GcFieldType>(i);
}

static QString
metadataFile()
{
    return QDir(gcroot).absoluteFilePath("metadata.xml");
}

struct MetadataConfig {
    QList<KeywordDefinition> keywords;
    QList<FieldDefinition> fields;
    QString colorfield;
    QList<DefaultDefinition> defaults;

    // the definitions as loaded, the copy activity set and the GUI's Data
    // Fields page use (from metadata.xml, older athlete files or the
    // built-in defaults)
    void read() {
        RideMetadata *metadata = GlobalContext::context()->rideMetadata;
        keywords = metadata->getKeywords();
        fields = metadata->getFields();
        colorfield = metadata->getColorField();
        defaults = metadata->getDefaults();
    }

    bool write(QString &error) {
        if (!RideMetadata::serialize(metadataFile(), keywords, fields, colorfield, defaults, &error)) return false;

        // everyone reads the new definitions, the in-memory copy included
        GlobalContext::context()->notifyConfigChanged(CONFIG_FIELDS);
        return true;
    }

    int find(const QString &name) const {
        for (int i = 0; i < fields.count(); i++) if (fields.at(i).name == name) return i;
        return -1;
    }
};

static QJsonObject
fieldJson(const FieldDefinition &f)
{
    QJsonObject o;
    o.insert("name", f.name);
    o.insert("type", typeName(f.type));
    o.insert("tab", f.tab);
    o.insert("numeric", f.isNumericField());
    if (f.diary) o.insert("summary", true);
    if (f.interval) o.insert("interval", true);
    if (!f.values.isEmpty()) o.insert("values", QJsonArray::fromStringList(f.values));
    if (!f.expression.isEmpty()) o.insert("expression", f.expression);
    return o;
}

static CommandResult
listFields(CommandEnvironment &env, const CommandRequest &request)
{
    CommandResult home = requireHome(env);
    if (!home.ok()) return home;

    MetadataConfig config;
    config.read();

    QString tab = request.args.value("tab").toString();
    QJsonArray list;
    for (const FieldDefinition &f : config.fields) {
        if (!tab.isEmpty() && f.tab != tab) continue;
        list.append(fieldJson(f));
    }
    QJsonObject data;
    data.insert("fields", list);
    // none until the definitions have been saved
    data.insert("file", QFile::exists(metadataFile()) ? QJsonValue(metadataFile()) : QJsonValue());
    return CommandResult::success(data);
}

static CommandResult
addFields(CommandEnvironment &env, const CommandRequest &request)
{
    CommandResult writable = sharedSettingsWritable(env);
    if (!writable.ok()) return writable;

    GcFieldType type = typeFromName(request.args.value("type").toString());
    QString tab = request.args.value("tab").toString();
    bool update = request.args.value("update").toBool(false);
    bool summary = request.args.value("summary").toBool(false);
    bool interval = request.args.value("interval").toBool(false);
    QStringList values;
    for (const QJsonValue &v : request.args.value("value").toArray()) values << v.toString();

    MetadataConfig config;
    config.read();

    QJsonArray report;
    int added = 0, changed = 0, unchanged = 0, failed = 0;
    QString text;

    for (const QJsonValue &v : request.args.value("name").toArray()) {
        QString name = v.toString().trimmed();
        QJsonObject r;
        r.insert("name", name);

        if (name.isEmpty() || name.contains("##") || name.contains('"')) {
            r.insert("status", "failed");
            r.insert("message", "invalid field name");
            failed++;
        } else {
            int i = config.find(name);
            if (i < 0) {
                config.fields.append(FieldDefinition(tab, name, type, summary, interval, values, ""));
                r.insert("status", "added");
                added++;
            } else {
                // type and tab always (they have defaults), the rest when given
                FieldDefinition &f = config.fields[i];
                bool setSummary = request.args.contains("summary");
                bool setInterval = request.args.contains("interval");
                bool setValues = request.args.contains("value");
                bool same = f.type == type && f.tab == tab
                            && (!setSummary || f.diary == summary)
                            && (!setInterval || f.interval == interval)
                            && (!setValues || f.values == values);
                if (same) {
                    r.insert("status", "unchanged");
                    unchanged++;
                } else if (!update) {
                    QStringList as;
                    as << QString("%1 on tab '%2'").arg(typeName(f.type)).arg(f.tab);
                    if (f.diary) as << "in the summary";
                    if (f.interval) as << "for intervals";
                    if (!f.values.isEmpty()) as << QString("with values %1").arg(f.values.join(", "));
                    r.insert("status", "failed");
                    r.insert("message", QString("exists as %1, use --update to change it").arg(as.join(", ")));
                    failed++;
                } else {
                    f.type = type;
                    f.tab = tab;
                    if (setSummary) f.diary = summary;
                    if (setInterval) f.interval = interval;
                    if (setValues) f.values = values;
                    r.insert("status", "updated");
                    changed++;
                }
            }
        }
        report.append(r);
        text += ResultFormat::statusLine(r, "name", 9);
    }

    if (added || changed) {
        QString error;
        if (!config.write(error)) return CommandResult::failure(Status::Failed, error);
    }

    QJsonObject data;
    data.insert("fields", report);
    data.insert("added", added);
    data.insert("updated", changed);
    data.insert("unchanged", unchanged);
    data.insert("failed", failed);
    CommandResult result = CommandResult::batch(data, failed, added + changed + unchanged + failed,
                                                QString("%1 field(s) not added").arg(failed));
    result.text = text;
    return result;
}

static CommandResult
removeFields(CommandEnvironment &env, const CommandRequest &request)
{
    CommandResult writable = sharedSettingsWritable(env);
    if (!writable.ok()) return writable;

    MetadataConfig config;
    config.read();

    QJsonArray removed;
    QStringList missing;
    for (const QJsonValue &v : request.args.value("name").toArray()) {
        int i = config.find(v.toString());
        if (i < 0) { missing << v.toString(); continue; }
        config.fields.removeAt(i);
        removed.append(v.toString());
    }
    if (!missing.isEmpty() && removed.isEmpty())
        return CommandResult::failure(Status::NotFound, QString("no such field: %1").arg(missing.join(", ")));

    if (!removed.isEmpty()) {
        QString error;
        if (!config.write(error)) return CommandResult::failure(Status::Failed, error);
    }

    QJsonObject data;
    data.insert("removed", removed);
    if (!missing.isEmpty()) data.insert("missing", QJsonArray::fromStringList(missing));
    CommandResult result = CommandResult::success(data);
    if (!missing.isEmpty()) {
        result.status = Status::Partial;
        result.error = QString("no such field: %1").arg(missing.join(", "));
    }
    return result;
}

void
registerFieldCommands(CommandRegistry &registry)
{
    Command list;
    list.spec.name = "field.list";
    list.spec.summary = "list the activity fields (Options > Data Fields)";
    list.spec.scope = Scope::Global;
    list.spec.params << ParamSpec("tab", ParamType::String, "only fields shown on this tab");
    list.spec.httpMethod = "GET";
    list.spec.httpPath = "/fields";
    list.handler = listFields;
    registry.add(list);

    Command add;
    add.spec.name = "field.add";
    add.spec.summary = "add activity fields, e.g. numeric inputs for a data processor";
    add.spec.description =
        "Adds one or more fields to the activity metadata definitions. Fields that\n"
        "already exist with the same type and tab are left alone, so the command can be\n"
        "run repeatedly. Field definitions are shared by all athletes in the athletes\n"
        "folder, as in the GUI.";
    add.spec.scope = Scope::Global;
    add.spec.modifies = true;
    add.spec.params << ParamSpec("name", ParamType::String, "field name").req().pos().many();
    add.spec.params << ParamSpec("type", ParamType::String, "field type").def("double").oneOf(typeNames);
    add.spec.params << ParamSpec("tab", ParamType::String, "tab the field is shown on in the GUI").def("Extra");
    add.spec.params << ParamSpec("summary", ParamType::Bool, "show in the activity summary (diary)");
    add.spec.params << ParamSpec("interval", ParamType::Bool, "field belongs to intervals rather than activities");
    add.spec.params << ParamSpec("value", ParamType::String, "suggested value for completion").many();
    add.spec.params << ParamSpec("update", ParamType::Bool, "change the type, tab, summary, interval or values of existing fields");
    add.spec.httpMethod = "POST";
    add.spec.httpPath = "/fields";
    add.handler = addFields;
    registry.add(add);

    Command remove;
    remove.spec.name = "field.remove";
    remove.spec.summary = "remove activity field definitions (values in activities are kept)";
    remove.spec.scope = Scope::Global;
    remove.spec.modifies = true;
    remove.spec.params << ParamSpec("name", ParamType::String, "field name").req().pos().many();
    remove.spec.httpMethod = "DELETE";
    remove.spec.httpPath = "/fields/{name}";
    remove.handler = removeFields;
    registry.add(remove);
}

} // namespace Headless
