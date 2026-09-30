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

#include "ProgramArgs.h"

#include "DataFilter.h"

#include <QFile>
#include <QTextStream>
#include <cstdio>

namespace Headless {

ParamSpec
programFileParam(const QString &description)
{
    return ParamSpec("file", ParamType::Path, description).cliOnly();
}

CommandResult
readProgramArg(const CommandRequest &request, bool required, QString &program)
{
    bool hasProgram = request.args.contains("program");
    bool hasFile = request.args.contains("file");
    if (hasProgram && hasFile)
        return CommandResult::failure(Status::Usage, "give the program with --program or --file, not both");
    if (!hasProgram && !hasFile) {
        if (required) return CommandResult::failure(Status::Usage, "give the program with --program or --file");
        return CommandResult::success();
    }

    if (hasFile) {
        QString path = request.args.value("file").toString();
        if (path == "-") {
            QTextStream in(stdin);
            program = in.readAll();
        } else {
            QFile in(path);
            if (!in.open(QIODevice::ReadOnly | QIODevice::Text))
                return CommandResult::failure(Status::NotFound, QString("can't read %1").arg(path));
            program = QTextStream(&in).readAll();
        }
    } else {
        program = request.args.value("program").toString();
    }
    return CommandResult::success();
}

CommandResult
checkProgram(Context *context, const QString &program, bool needValueBlock)
{
    if (program.trimmed().isEmpty()) return CommandResult::failure(Status::Usage, "program is empty");

    DataFilter checker(nullptr, context);
    QStringList errors = checker.check(program);
    if (!errors.isEmpty() || !checker.root()) {
        if (errors.isEmpty()) errors << QString("malformed expression.");
        return CommandResult::failure(Status::Usage, errors.join("\n"));
    }
    if (needValueBlock && !checker.rt.functions.contains("value"))
        return CommandResult::failure(Status::Usage, "program needs a value block");
    return CommandResult::success();
}

} // namespace Headless
