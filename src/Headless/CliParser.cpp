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

#include "CliParser.h"

#include <QSet>
#include <QTextStream>

namespace Headless {

// global options that take a value
static const QStringList valueOptions = {
    "--home", "--athlete", "--athlete-dir", "--format", "--output", "--lock-wait"
};

// global flags
static const QStringList flagOptions = {
    "--quiet", "--verbose", "--no-python", "--force", "--help", "--version"
};

static QString
expandShort(const QString &arg)
{
    if (arg == "-h") return "--help";
    if (arg == "-q") return "--quiet";
    if (arg == "-v") return "--verbose";
    if (arg == "-o") return "--output";
    if (arg == "-f") return "--format";
    if (arg == "-a") return "--athlete";
    return arg;
}

// append a raw value, turning repeats into an array
static void
addValue(QJsonObject &args, const QString &name, const QJsonValue &value)
{
    if (!args.contains(name)) {
        args.insert(name, value);
        return;
    }
    QJsonValue existing = args.value(name);
    QJsonArray list = existing.isArray() ? existing.toArray() : QJsonArray{ existing };
    list.append(value);
    args.insert(name, list);
}

static bool
applyGlobal(GlobalOptions &g, CliParse &out, const QString &name, const QString &value)
{
    if (name == "--home") g.home = value;
    else if (name == "--athlete") g.athlete = value;
    else if (name == "--athlete-dir") g.athleteDir = value;
    else if (name == "--output") g.output = value;
    else if (name == "--format") {
        QString f = value.toLower();
        if (f != "text" && f != "json") {
            out.error = QString("--format must be 'text' or 'json', not '%1'").arg(value);
            return false;
        }
        g.format = f;
    } else if (name == "--lock-wait") {
        bool ok = false;
        int secs = value.toInt(&ok);
        if (!ok || secs < 0) {
            out.error = QString("--lock-wait expects a number of seconds, not '%1'").arg(value);
            return false;
        }
        g.lockWait = secs;
    }
    else if (name == "--quiet") g.quiet = true;
    else if (name == "--verbose") g.verbose = true;
    else if (name == "--no-python") g.noPython = true;
    else if (name == "--force") g.force = true;
    else if (name == "--help") out.help = true;
    else if (name == "--version") out.version = true;
    return true;
}

CliParse
CliParser::parse(const QStringList &argv, const CommandRegistry &registry)
{
    CliParse out;
    QStringList words;              // command words and positionals, in order
    QList<QPair<QString,QString>> options; // command options, name and raw value (null = no value)
    QList<bool> optionHasValue;
    bool endOfOptions = false;

    // first pass: split global options from everything else
    for (int i = 0; i < argv.count(); i++) {
        QString arg = argv.at(i);

        if (endOfOptions || !arg.startsWith("-") || arg == "-") {
            words << arg;
            continue;
        }
        if (arg == "--") {
            endOfOptions = true;
            continue;
        }

        arg = expandShort(arg);

        QString name = arg, value;
        bool hasValue = false;
        int eq = arg.indexOf('=');
        if (arg.startsWith("--") && eq > 2) {
            name = arg.left(eq);
            value = arg.mid(eq + 1);
            hasValue = true;
        }

        if (valueOptions.contains(name)) {
            if (!hasValue) {
                if (i + 1 >= argv.count()) {
                    out.error = QString("%1 needs a value").arg(name);
                    return out;
                }
                value = argv.at(++i);
            }
            if (!applyGlobal(out.global, out, name, value)) return out;
            continue;
        }
        if (flagOptions.contains(name) && !hasValue) {
            applyGlobal(out.global, out, name, QString());
            continue;
        }

        // a command option, resolved once we know the command. We don't know
        // yet if it takes a value, so remember the position
        options << qMakePair(name, hasValue ? value : QString());
        optionHasValue << hasValue;

        // peek: an option without '=' may consume the next word as its value,
        // decided after the command is known. Record a marker in words.
        words << QString("\x01%1").arg(options.count() - 1);
    }

    // "help [topic...]" is an alias for --help
    if (!words.isEmpty() && words.first() == "help") {
        out.help = true;
        words.removeFirst();
    }

    // find the longest run of leading words that names a command or group
    QStringList plain;
    for (const QString &w : words) {
        if (w.startsWith("\x01")) break;
        plain << w;
    }

    int used = 0;
    for (int n = plain.count(); n > 0; n--) {
        QString candidate = plain.mid(0, n).join(".");
        if (registry.find(candidate)) {
            out.command = candidate;
            used = n;
            break;
        }
    }

    if (out.command.isEmpty()) {
        // maybe a group ("activity") or nothing at all
        if (!plain.isEmpty()) {
            for (int n = plain.count(); n > 0; n--) {
                QString candidate = plain.mid(0, n).join(".");
                if (!registry.namesWithPrefix(candidate).isEmpty()) {
                    out.helpTopic = candidate;
                    if (!out.help) out.error = QString("'%1' needs a subcommand").arg(plain.mid(0, n).join(" "));
                    return out;
                }
            }
            out.error = QString("unknown command '%1'").arg(plain.join(" "));
            return out;
        }
        if (!options.isEmpty() && !out.help) out.error = QString("unknown option '%1'").arg(options.first().first);
        return out;
    }

    if (out.help) {
        out.helpTopic = out.command;
        return out;
    }

    const CommandSpec &spec = registry.find(out.command)->spec;

    // positional parameters in declaration order
    QList<const ParamSpec *> positionals;
    for (const ParamSpec &p : spec.params) if (p.positional) positionals << &p;
    int nextPositional = 0;

    // second pass over what follows the command words
    for (int i = used; i < words.count(); i++) {
        const QString &w = words.at(i);

        if (w.startsWith("\x01")) {
            int index = w.mid(1).toInt();
            QString name = options.at(index).first;
            QString value = options.at(index).second;
            bool hasValue = optionHasValue.at(index);

            // --no-flag for booleans
            QString pname = name.mid(2);
            const ParamSpec *p = spec.param(pname);
            if (!p && pname.startsWith("no-")) {
                const ParamSpec *neg = spec.param(pname.mid(3));
                if (neg && neg->type == ParamType::Bool && !hasValue) {
                    addValue(out.args, neg->name, QString("false"));
                    continue;
                }
            }
            if (!p || !name.startsWith("--")) {
                out.error = QString("unknown option '%1' for '%2'").arg(name).arg(spec.cliName());
                return out;
            }

            if (p->type == ParamType::Bool) {
                addValue(out.args, p->name, hasValue ? value : QString("true"));
                continue;
            }
            if (!hasValue) {
                // take the next plain word as the value
                if (i + 1 >= words.count() || words.at(i + 1).startsWith("\x01")) {
                    out.error = QString("%1 needs a value").arg(name);
                    return out;
                }
                value = words.at(++i);
            }
            addValue(out.args, p->name, value);
            continue;
        }

        // positional
        if (nextPositional >= positionals.count()) {
            out.error = QString("unexpected argument '%1' for '%2'").arg(w).arg(spec.cliName());
            return out;
        }
        const ParamSpec *p = positionals.at(nextPositional);
        addValue(out.args, p->name, w);
        if (!p->repeated) nextPositional++;
    }

    return out;
}

static QString
paramSynopsis(const ParamSpec &p)
{
    QString s;
    if (p.positional) {
        s = p.name.toUpper();
        if (p.repeated) s += "...";
    } else if (p.type == ParamType::Bool) {
        s = "--" + p.name;
    } else {
        s = QString("--%1 %2").arg(p.name).arg(p.name == "format" ? QString("FMT") : paramTypeName(p.type).toUpper());
        if (p.repeated) s += "...";
    }
    if (!p.required) s = "[" + s + "]";
    return s;
}

QString
CliParser::commandHelp(const CommandSpec &spec, const QString &program)
{
    QString text;
    QTextStream out(&text);

    QStringList synopsis;
    for (const ParamSpec &p : spec.params) synopsis << paramSynopsis(p);

    out << "Usage: " << program << " " << spec.cliName();
    if (!synopsis.isEmpty()) out << " " << synopsis.join(" ");
    out << "\n\n" << spec.summary << "\n";
    if (!spec.description.isEmpty()) out << "\n" << spec.description << "\n";

    if (!spec.params.isEmpty()) {
        out << "\nParameters:\n";
        for (const ParamSpec &p : spec.params) {
            QString left = p.positional ? p.name.toUpper() : ("--" + p.name);
            QString line = QString("  %1").arg(left, -22);
            if (left.length() > 20) line += "\n" + QString(24, ' ');
            line += p.description;
            QStringList notes;
            if (p.required) notes << "required";
            if (p.repeated) notes << "repeatable";
            if (!p.choices.isEmpty()) notes << "one of: " + p.choices.join(", ");
            if (!p.defaultValue.isUndefined() && !p.defaultValue.isNull())
                notes << "default: " + p.defaultValue.toVariant().toString();
            if (!notes.isEmpty()) line += " (" + notes.join("; ") + ")";
            out << line << "\n";
        }
    }
    if (!spec.httpPath.isEmpty())
        out << "\nREST: " << spec.httpMethod << " /v1" << spec.httpPath << "\n";
    return text;
}

QString
CliParser::groupHelp(const CommandRegistry &registry, const QString &group, const QString &program)
{
    QString text;
    QTextStream out(&text);
    out << "Usage: " << program << " " << QString(group).replace('.', ' ') << " <command> [options]\n\nCommands:\n";
    for (const QString &name : registry.namesWithPrefix(group)) {
        const Command *c = registry.find(name);
        out << QString("  %1").arg(c->spec.cliName(), -28) << c->spec.summary << "\n";
    }
    out << "\nRun '" << program << " help <command>' for details.\n";
    return text;
}

QString
CliParser::usage(const CommandRegistry &registry, const QString &program)
{
    QString text;
    QTextStream out(&text);
    out << "Usage: " << program << " [global options] <command> [options]\n\n"
        << "Run GoldenCheetah without its window: import activities, run data\n"
        << "processors, query metrics and export charts. Every command opens the\n"
        << "athlete, brings its cache up to date with the files on disk, does its\n"
        << "work, saves and closes the athlete again before exiting.\n\n"
        << "Global options:\n"
        << "  --home DIR              athletes folder (default: the GoldenCheetah library folder)\n"
        << "  -a, --athlete NAME      athlete folder name inside --home\n"
        << "  --athlete-dir DIR       full path of an athlete folder (sets --home and --athlete)\n"
        << "  -f, --format FMT        output format: text (default) or json\n"
        << "  -o, --output FILE       write binary output (charts, exports) here, '-' for stdout\n"
        << "  -q, --quiet             print errors only\n"
        << "  -v, --verbose           print GoldenCheetah diagnostics to stderr\n"
        << "  --lock-wait SECS        wait up to SECS for another process to release the athlete\n"
        << "  --force                 open an athlete that GoldenCheetah did not close cleanly\n"
        << "  --no-python             do not start embedded Python\n"
        << "  -h, --help              show help, 'help <command>' for a command\n"
        << "  --version               print version information\n\n"
        << "Commands:\n";

    // group by first word
    QString lastGroup;
    for (const Command *c : registry.commands()) {
        QString group = c->spec.name.section('.', 0, 0);
        if (group != lastGroup && !lastGroup.isEmpty()) out << "\n";
        lastGroup = group;
        out << QString("  %1").arg(c->spec.cliName(), -28) << c->spec.summary << "\n";
    }
    out << "\nExit status: 0 ok, 1 some items failed, 2 usage error, 3 not found,\n"
        << "4 athlete in use, 5 failed, 6 internal error.\n";
    return text;
}

} // namespace Headless
