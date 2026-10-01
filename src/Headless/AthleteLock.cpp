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

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSysInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
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

QString
AthleteLock::lockFilePath(const QString &athleteDir)
{
    return canonicalKey(athleteDir) + "/athlete.lock";
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

    QMutexLocker locker(&registryMutex);

    // already held by this process, share it
    LockEntry &entry = registry()[key];
    if (entry.count > 0) {
        entry.count++;
        locked = true;
        return true;
    }

    std::shared_ptr<QLockFile> file = std::make_shared<QLockFile>(lockFilePath(key));

    // never consider a lock stale just because it is old, a long running
    // GUI session is legitimate. QLockFile still removes locks whose owning
    // process has died.
    file->setStaleLockTime(0);

    if (file->tryLock(timeoutMs)) {
        entry.count = 1;
        entry.file = file;
        locked = true;
        pid = 0;
        return true;
    }

    // remember who has it for diagnostics
    registry().remove(key);
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
