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

#ifndef _GC_ProgramArgs_h
#define _GC_ProgramArgs_h 1

#include "HeadlessCommand.h"

class Context;

namespace Headless {

//
// Formula programs given to a command (a user metric, an overview tile):
// the text with --program, or read from --file (- for stdin). Reading a
// file is for the command line only, the REST API sends the text.
//

// the --file parameter that goes with --program
ParamSpec programFileParam(const QString &description);

// the program the request gives; with required, one of the two must be
// there. Fails on both, or on a file that can't be read.
CommandResult readProgramArg(const CommandRequest &request, bool required, QString &program);

// the formula editor's parse. A user metric computes its value block, so it
// must have one (a program without would compute nothing and say nothing).
CommandResult checkProgram(Context *context, const QString &program, bool needValueBlock);

} // namespace Headless

#endif
