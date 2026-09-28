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

#include "HeadlessApp.h"

#include "Settings.h"
#include "Colors.h"
#include "RideMetric.h"
#include "PowerProfile.h"
#include "TrainDB.h"
#include "Context.h"

#ifdef GC_WANT_PYTHON
#include "PythonEmbed.h"
#include "FixPySettings.h"
#endif

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QProcessEnvironment>
#include <cstdio>

#include <gsl/gsl_errno.h>

// globals owned by main.cpp
extern QString gcroot;
extern QApplication *application;

namespace Headless {

static bool initialised = false;
static HeadlessApp::Options lastOptions;
static bool verboseMessages = false;
static QString rootFolder;

// GoldenCheetah is chatty on qDebug/qWarning, a command line tool must not be
static void
messageHandler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    if (!verboseMessages && type != QtCriticalMsg && type != QtFatalMsg) return;

    const char *label = "debug";
    switch (type) {
    case QtDebugMsg: label = "debug"; break;
    case QtInfoMsg: label = "info"; break;
    case QtWarningMsg: label = "warning"; break;
    case QtCriticalMsg: label = "critical"; break;
    case QtFatalMsg: label = "fatal"; break;
    }
    fprintf(stderr, "[gc %s] %s\n", label, msg.toLocal8Bit().constData());
    fflush(stderr);
}

void
HeadlessApp::createApplication(int &argc, char **argv)
{
    if (application) return;

    // no display is needed or wanted, but the caller may override. Qt tries
    // the platforms in order, a deployed app may not ship the offscreen one
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
#if defined(Q_OS_MACOS)
        qputenv("QT_QPA_PLATFORM", "offscreen;cocoa");
#elif defined(Q_OS_WIN)
        qputenv("QT_QPA_PLATFORM", "offscreen;windows");
#else
        qputenv("QT_QPA_PLATFORM", "offscreen;minimal;xcb;wayland");
#endif
    }

    // offscreen rendering has no fonts configured by default on some systems
    // and complains loudly, the GUI does the same unset
#ifdef Q_OS_LINUX
    unsetenv("QT_SCALE_FACTOR");
#endif

    qInstallMessageHandler(messageHandler);

    // from here on nothing may wait for a user to click a dialog away
    GlobalContext::setHeadless(true);

    // widgets are still needed: metadata and charts are QWidgets
    application = new QApplication(argc, argv);
    application->setApplicationName("GoldenCheetah");
}

QString
HeadlessApp::defaultHome()
{
    // explicit override for scripts and containers
    QString env = QProcessEnvironment::systemEnvironment().value("GC_HOME");
    if (!env.isEmpty()) return QDir::cleanPath(QFileInfo(env).absoluteFilePath());

    // configured library folder
    QString configured = appsettings->value(NULL, GC_HOMEDIR, "").toString();
    if (!configured.isEmpty() && QDir(configured).exists()) return QDir(configured).canonicalPath();

    // the same search as main()
    QString old = QDir::home().canonicalPath() + "/Library/GoldenCheetah";
    if (QDir(old).exists()) return old;

#if defined(Q_OS_MACOS)
    return QDir::home().canonicalPath() + "/Library/GoldenCheetah";
#elif defined(Q_OS_WIN)
    QStringList paths = QStandardPaths::standardLocations(QStandardPaths::AppLocalDataLocation);
    return paths.value(0);
#else
    return QDir::home().canonicalPath() + "/.goldencheetah";
#endif
}

QStringList
HeadlessApp::athletes(const QString &home)
{
    QStringList list;
    QDir dir(home);
    for (const QString &name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (name.startsWith(".")) continue;
        QDir athlete(dir.absoluteFilePath(name));
        if (athlete.exists("config") || athlete.exists("activities")) list << name;
    }
    return list;
}

bool
HeadlessApp::isInitialised()
{
    return initialised;
}

QString
HeadlessApp::home()
{
    return rootFolder;
}

void
HeadlessApp::setOptions(const Options &options)
{
    lastOptions = options;
    verboseMessages = options.verbose;
}

bool
HeadlessApp::initialise(const QString &home, QString &error)
{
    return initialise(home, lastOptions, error);
}

bool
HeadlessApp::initialise(const QString &home, const Options &options, QString &error)
{
    setOptions(options);

    QFileInfo info(home);
    if (!info.exists() || !info.isDir()) {
        error = QString("athletes folder '%1' does not exist").arg(home);
        return false;
    }
    QString canonical = info.canonicalFilePath();

    if (initialised) {
        if (canonical != rootFolder) {
            error = QString("already using athletes folder '%1'").arg(rootFolder);
            return false;
        }
        return true;
    }

    // nobody is there to answer a dialog
    GlobalContext::setHeadless(true);

    // maths routines must not abort the process
    gsl_set_error_handler_off();

    // same order as main()
    initPowerProfile();

    rootFolder = canonical;
    gcroot = canonical;
    appsettings->initializeQSettingsGlobal(gcroot);

    GCColor::setupColors();
    QString powercolor = appsettings->value(NULL, "COLORPOWER", "").toString();
    if (powercolor == "") GCColor::applyTheme(GSettings::defaultAppearanceSettings().theme);
    appsettings->migrateQSettingsSystem();
    GCColor::readConfig();

    RideMetricFactory::instance().initialize();

    // the upgrade code consults the workout database
    trainDB = new TrainDB(QDir(gcroot));

#ifdef GC_WANT_PYTHON
    bool embed = appsettings->value(NULL, GC_EMBED_PYTHON, true).toBool();
    if (options.python && embed && python == nullptr) {
        python = new PythonEmbed();
        if (python->loaded == false) python = nullptr;
    }
#endif

    // metadata definitions etc, read now so the root folder is used
    GlobalContext::context();

    initialised = true;
    return true;
}

bool
HeadlessApp::pythonAvailable()
{
#ifdef GC_WANT_PYTHON
    return python != nullptr;
#else
    return false;
#endif
}

void
HeadlessApp::shutdown()
{
    if (!initialised) return;

    delete trainDB;
    trainDB = nullptr;

    // flush settings to disk
    appsettings->syncQSettings();
    initialised = false;
}

} // namespace Headless
