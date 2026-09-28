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

#ifndef _GC_CliMain_h
#define _GC_CliMain_h 1

namespace Headless {

// is this a command line (headless) invocation? true for
// "GoldenCheetah --cli ..." and when started as gc-cli / goldencheetah-cli
bool isCliInvocation(int argc, char **argv);

// run the command line and return the exit status, never returns to the GUI
int cliMain(int argc, char **argv);

} // namespace Headless

#endif
