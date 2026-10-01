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

#include "AthleteLock.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QThread>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSysInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <algorithm>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#endif

// in-process holders, keyed by canonical athlete path. The QLockFile is
// owned by the registry so it is released when the last holder unlocks,
// whatever order the holders are destroyed in.
struct LockEntry {
    int count = 0;
    std::shared_ptr<QLockFile> file;
};

static QMutex registryMutex;
static QHash<QString, LockEntry> &registry()
{
    static QHash<QString, LockEntry> entries;
    return entries;
}

static QString canonicalKey(const QString &athleteDir)
{
    QFileInfo info(athleteDir);
    QString path = info.canonicalFilePath();
    if (path.isEmpty()) path = info.absoluteFilePath();
    return QDir::cleanPath(path);
}

// a folder on this machine, never in the athlete folder: athlete folders
// are often synced (Dropbox, OneDrive), and a lock synced from another
// machine would never be stale here. $XDG_RUNTIME_DIR where it's set (it
// is cleared at logout), else the user's cache folder, else temp.
static QString
lockDirectory()
{
    static QString dir;
    static QMutex mutex;
    QMutexLocker locker(&mutex);
    if (!dir.isEmpty()) return dir;

    QStringList bases;
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!runtime.isEmpty() && QDir::isAbsolutePath(runtime)) bases << runtime;
    QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (!cache.isEmpty()) bases << cache;
    bases << QDir::tempPath();
    for (const QString &base : bases) {
        QString candidate = QDir(base).absoluteFilePath("GoldenCheetah/locks");
        if (QDir().mkpath(candidate) && QFileInfo(candidate).isWritable()) return dir = candidate;
    }
    return dir = QDir(QDir::tempPath()).absoluteFilePath("GoldenCheetah/locks");
}

QString
AthleteLock::lockFilePath(const QString &athleteDir)
{
    QString key = canonicalKey(athleteDir);
#ifdef Q_OS_WIN
    // one lock whatever the spelling: long names for 8.3 ones (RUNNER~1),
    // and case doesn't matter
    std::wstring native = QDir::toNativeSeparators(key).toStdWString();
    DWORD size = GetLongPathNameW(native.c_str(), nullptr, 0);
    if (size) {
        std::wstring longName(size, L'\0');
        DWORD got = GetLongPathNameW(native.c_str(), &longName[0], size);
        if (got && got < size) key = QDir::fromNativeSeparators(QString::fromStdWString(longName.substr(0, got)));
    }
    key = key.toLower();
#endif
    QByteArray hash = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    return lockDirectory() + "/" + QString::fromLatin1(hash) + ".lock";
}

// earlier builds of this branch kept the lock in the athlete folder;
// remove one left behind when its process is gone
static void
removeOldLock(const QString &key)
{
    QString old = key + "/athlete.lock";
    if (!QFile::exists(old)) return;
    // only a stale one can be taken, and unlocking removes it
    QLockFile file(old);
    file.setStaleLockTime(0);
    if (file.tryLock(0)) file.unlock();
}

bool
AthleteLock::heldByThisProcess(const QString &athleteDir)
{
    QMutexLocker locker(&registryMutex);
    return registry().value(canonicalKey(athleteDir)).count > 0;
}

// is there a process with this id on this machine (Qt has no public API)
static bool
processAlive(qint64 pid)
{
    if (pid <= 0) return false;
#ifdef Q_OS_WIN
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

AthleteLock::State
AthleteLock::peek(const QString &athleteDir, QString *holder)
{
    if (heldByThisProcess(athleteDir)) return State::Free;

    QString path = lockFilePath(athleteDir);
    if (!QFile::exists(path)) return State::Free;

    qint64 pid = 0;
    QString hostname, appname;
    QLockFile file(path);
    // being written this moment, or unreadable: assume it's held
    if (!file.getLockInfo(&pid, &hostname, &appname)) {
        if (holder) *holder = describe(0, QString(), QString());
        return State::InUse;
    }

    // a dead local process's lock is stale, the next tryLock removes it.
    // Another machine's can't be checked, so it counts, as QLockFile has it.
    bool local = hostname.isEmpty() || hostname == QSysInfo::machineHostName();
    if (local && !processAlive(pid)) return State::Free;
    if (holder) *holder = describe(pid, hostname, appname);
    return State::InUse;
}

AthleteLock::AthleteLock(const QString &athleteDir) : key(canonicalKey(athleteDir))
{
}

AthleteLock::~AthleteLock()
{
    unlock();
}

bool
AthleteLock::tryLock(int timeoutMs)
{
    if (locked) return true;

    std::shared_ptr<QLockFile> file = std::make_shared<QLockFile>(lockFilePath(key));

    // never consider a lock stale just because it is old, a long running
    // GUI session is legitimate. QLockFile still removes locks whose owning
    // process has died.
    file->setStaleLockTime(0);

    // in steps, the registry free in between: another thread of this
    // process may take the lock meanwhile, and then it's shared
    QElapsedTimer waited;
    waited.start();
    for (;;) {
        {
            QMutexLocker locker(&registryMutex);

            // already held by this process, share it
            auto it = registry().find(key);
            if (it != registry().end() && it->count > 0) {
                it->count++;
                locked = true;
                return true;
            }

            if (file->tryLock(0)) {
                removeOldLock(key);
                LockEntry &entry = registry()[key];
                entry.count = 1;
                entry.file = file;
                locked = true;
                pid = 0;
                return true;
            }
        }
        qint64 left = timeoutMs - waited.elapsed();
        if (left <= 0) break;
        QThread::msleep(ulong(std::min<qint64>(left, 100)));
    }

    // remember who has it for diagnostics
    pid = 0;
    hostname.clear();
    appname.clear();
    file->getLockInfo(&pid, &hostname, &appname);
    return false;
}

void
AthleteLock::unlock()
{
    if (!locked) return;

    QMutexLocker locker(&registryMutex);

    auto it = registry().find(key);
    if (it != registry().end() && --(it->count) <= 0) {
        if (it->file) it->file->unlock();
        registry().erase(it);
    }
    locked = false;
}

QString
AthleteLock::holder() const
{
    return describe(pid, hostname, appname);
}

QString
AthleteLock::describe(qint64 pid, const QString &hostname, const QString &appname)
{
    if (pid == 0) return QString("another process");

    QString who = appname.isEmpty() ? QString("process") : appname;
    if (hostname.isEmpty()) return QString("%1 (pid %2)").arg(who).arg(pid);
    return QString("%1 (pid %2 on %3)").arg(who).arg(pid).arg(hostname);
}
