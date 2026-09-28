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

#ifndef _GC_AthleteLock_h
#define _GC_AthleteLock_h 1

#include <QString>
#include <QLockFile>

//
// Inter-process lock on an athlete folder.
//
// The GUI takes this lock when it opens an athlete and the command line
// and REST entry points take it for the duration of a command, so that two
// processes never modify the same athlete folder at the same time.
//
// Within one process the lock is reference counted: an Athlete opened by a
// headless session that already holds the lock shares it, rather than
// deadlocking against itself.
//
// A lock left behind by a process that no longer exists is detected as
// stale and removed automatically (see QLockFile).
//
class AthleteLock
{
    public:

        explicit AthleteLock(const QString &athleteDir);
        ~AthleteLock();

        AthleteLock(const AthleteLock&) = delete;
        AthleteLock& operator=(const AthleteLock&) = delete;

        // try to acquire, waiting up to timeoutMs (0 = don't wait)
        // returns true if this process now holds the lock
        bool tryLock(int timeoutMs = 0);

        // release (only if we acquired it)
        void unlock();

        bool isLocked() const { return locked; }

        // who holds the lock when tryLock() failed, human readable
        QString holder() const;
        qint64 holderPid() const { return pid; }

        // the file used for locking
        static QString lockFilePath(const QString &athleteDir);

        // does this process already hold the lock for this athlete?
        static bool heldByThisProcess(const QString &athleteDir);

    private:

        QString key;          // canonical athlete path
        bool locked = false;
        qint64 pid = 0;
        QString hostname, appname;
};

#endif
