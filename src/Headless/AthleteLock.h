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
// The lock file is kept on this machine, not in the athlete folder, so a
// folder synced between machines never carries a lock from one to the
// other. It excludes processes of the same user on the same machine.
//
// Processes on different systems can't see each other's lock file: Windows
// and WSL on one computer, a container and its host, two computers sharing
// a folder. For those the holder also leaves a note in the athlete folder
// (in-use.json, removed when it unlocks). A note from another system can't
// be checked, so it stops the lock being taken until that system removes
// it, or the user says the athlete isn't open there after all (force).
//
class AthleteLock
{
    public:

        explicit AthleteLock(const QString &athleteDir);
        ~AthleteLock();

        AthleteLock(const AthleteLock&) = delete;
        AthleteLock& operator=(const AthleteLock&) = delete;

        // try to acquire, waiting up to timeoutMs (0 = don't wait)
        // returns true if this process now holds the lock. With force a
        // note left in the athlete folder by another system is overruled.
        bool tryLock(int timeoutMs = 0, bool force = false);

        // release (only if we acquired it)
        void unlock();

        bool isLocked() const { return locked; }

        // who holds the lock when tryLock() failed, human readable
        QString holder() const;
        static QString describe(qint64 pid, const QString &hostname, const QString &appname,
                                const QString &system = QString());
        qint64 holderPid() const { return pid; }

        // tryLock() failed because of another system's note, which can't be
        // checked: the holder may have gone without removing it
        bool heldElsewhere() const { return elsewhere; }

        // the file used for locking, named by a hash of the folder's path
        static QString lockFilePath(const QString &athleteDir);

        // another file of this machine's for the athlete, next to the lock,
        // and the folder they are in
        static QString runtimeFilePath(const QString &athleteDir, const QString &suffix);
        static QString runtimeDirectory();

        // is there a process with this id on this machine
        static bool processRunning(qint64 pid);

        // the note left in the athlete folder for other systems
        static QString notePath(const QString &athleteDir);

        // this system, as the note identifies it: a hash of the machine id
        // and kernel (WSL has its Windows host's computer name)
        static QString systemId();

        // how the note names this process, e.g. "GoldenCheetah command line"
        // (default: the application name)
        static void setHolderName(const QString &name);

        // does this process already hold the lock for this athlete?
        static bool heldByThisProcess(const QString &athleteDir);

        // is the athlete locked by another live process? Reads the lock
        // file without taking the lock, so a GUI opening the athlete at
        // the same moment isn't turned away. holder as holder() has it.
        enum class State { Free, InUse };
        static State peek(const QString &athleteDir, QString *holder = nullptr);

    private:

        QString key;          // canonical athlete path
        bool locked = false;
        qint64 pid = 0;
        QString hostname, appname, system;
        bool elsewhere = false;
};

#endif
