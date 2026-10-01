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

#include "GcStartup.h"

#include "Settings.h"
#include "Colors.h"
#include "RideMetric.h"
#include "PowerProfile.h"
#include "TrainDB.h"

#include <QDir>
#include <QStandardPaths>

#include <gsl/gsl_errno.h>

extern QString gcroot;

QString
GcStartup::defaultLibraryPath()
{
    //these are the new platform-dependent library paths
#if defined(Q_OS_MACOS)
    return "Library/GoldenCheetah";
#elif defined(Q_OS_WIN)
    QStringList paths=QStandardPaths::standardLocations(QStandardPaths::AppLocalDataLocation);
    return paths.at(0);
#else // not windows or osx (must be Linux or OpenBSD)
    // Q_OS_LINUX et al
    return ".goldencheetah";
#endif //
}

QString
GcStartup::libraryPath(bool create)
{
    //this is the path within the current directory where GC will look for
    //files to allow USB stick support
    QString localLibraryPath="Library/GoldenCheetah";

    //this is the path that used to be used for all platforms
    //now different platforms will use their own path
    //this path is checked first to make things easier for long-time users
    QString oldLibraryPath=QDir::home().canonicalPath()+"/Library/GoldenCheetah";

    //these are the new platform-dependent library paths
    QString libraryPath = defaultLibraryPath();

    // or did we override in settings?
    QString sh;
    if ((sh=appsettings->value(NULL, GC_HOMEDIR, "").toString()) != QString("")) localLibraryPath = sh;

    // lets try the local library we've worked out...
    QDir home = QDir();
    if(QDir(localLibraryPath).exists() || home.exists(localLibraryPath)) {

        home.cd(localLibraryPath);

    } else {

        // YIKES !! The directory we should be using doesn't exist!
        home = QDir::home();
        if (home.exists(oldLibraryPath)) { // there is an old style path, lets fo there
            home.cd(oldLibraryPath);
        } else {

            if (!home.exists(libraryPath)) {
                if (!create) return QDir::cleanPath(home.absoluteFilePath(libraryPath));
                if (!home.mkpath(libraryPath)) return QString();
            }
            home.cd(libraryPath);
        }
    }
    return home.canonicalPath();
}

void
GcStartup::initProcess()
{
    // we don't want program aborts when maths routines don't know
    // what to do. We may add our own error handler later.
    gsl_set_error_handler_off();

    // read defaults
    initPowerProfile();
}

void
GcStartup::initSettings(const QString &home)
{
    // set global root directory
    gcroot = home;
    appsettings->initializeQSettingsGlobal(gcroot);
}

void
GcStartup::initCore(const QString &home)
{
    // Now the translator is installed, set default colors with translated names
    GCColor::setupColors();

    // has a default theme been applied (first run) ?
    QString powercolor = appsettings->value(NULL, "COLORPOWER", "").toString();
    if (powercolor == "")  GCColor::applyTheme(GSettings::defaultAppearanceSettings().theme);

    // migration
    appsettings->migrateQSettingsSystem(); // colors must be setup before migration can take place, but reading has to be from the migrated ones
    GCColor::readConfig();

    // Initialize metrics once the translator is installed
    RideMetricFactory::instance().initialize();

    // initialise the trainDB
    trainDB = new TrainDB(QDir(home));
}
