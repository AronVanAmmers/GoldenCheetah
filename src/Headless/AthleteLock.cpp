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
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <memory>

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
    if (pid == 0) return QString("another process");

    QString who = appname.isEmpty() ? QString("process") : appname;
    if (hostname.isEmpty()) return QString("%1 (pid %2)").arg(who).arg(pid);
    return QString("%1 (pid %2 on %3)").arg(who).arg(pid).arg(hostname);
}
