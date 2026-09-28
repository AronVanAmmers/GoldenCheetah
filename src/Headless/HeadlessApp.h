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

#ifndef _GC_HeadlessApp_h
#define _GC_HeadlessApp_h 1

#include <QString>
#include <QStringList>

namespace Headless {

//
// Process wide initialisation for headless use, the equivalent of the
// start-up part of main() that the GUI performs before opening a window:
// settings, metrics, colours, the train database and embedded Python.
//
// Everything is done at most once per process and for one athletes folder
// (GoldenCheetah keeps its root folder in a global).
//
class HeadlessApp
{
    public:

        struct Options {
            bool python = true;     // start embedded Python (if compiled in)
            bool verbose = false;   // let GoldenCheetah diagnostics through
        };

        // create the QApplication, using the offscreen platform unless the
        // caller chose one. Must be called before anything else.
        static void createApplication(int &argc, char **argv);

        // initialise for the given athletes root folder, returns false and
        // sets error when the folder can't be used
        static bool initialise(const QString &home, const Options &options, QString &error);

        static bool isInitialised();

        // the athletes folder GoldenCheetah would use: $GC_HOME, the configured
        // library folder, or the platform default
        static QString defaultHome();

        // the initialised athletes folder
        static QString home();

        // folders in home that look like athletes, sorted
        static QStringList athletes(const QString &home);

        // is embedded python running?
        static bool pythonAvailable();

        // tidy up process wide state before exit
        static void shutdown();
};

} // namespace Headless

#endif
