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

#ifndef _GC_GcStartup_h
#define _GC_GcStartup_h 1

#include <QString>

//
// The start-up main() does before it opens a window, shared with the
// command line and REST server (Headless::HeadlessApp), so both find the
// same athletes folder and set up the same way.
//
namespace GcStartup
{
    // the athletes folder ("library") as GoldenCheetah finds it: the one set
    // in the preferences, ./Library/GoldenCheetah (a library on a USB
    // stick), ~/Library/GoldenCheetah (older versions), or else the
    // platform's default. With create the default is made when it doesn't
    // exist yet; "" when that fails.
    QString libraryPath(bool create);

    // the platform's default, relative to the home folder (absolute on Windows)
    QString defaultLibraryPath();

    // once per process: maths errors don't abort, the power profile defaults
    void initProcess();

    // the athletes folder's global settings (gcroot is set to home)
    void initSettings(const QString &home);

    // once the settings are read (and in the GUI the translators installed):
    // colours and theme, settings migration, the metrics and the workout
    // database
    void initCore(const QString &home);
}

#endif
