#include "AthleteLock.h"

#include <QTest>
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

// run as a helper process: take the lock on HOLD_LOCK and wait to be killed
static int holdLock(const QString &dir)
{
    AthleteLock lock(dir);
    if (!lock.tryLock(0)) return 3;
    fprintf(stdout, "locked\n");
    fflush(stdout);
    for (;;) QThread::sleep(1);
}

static void writeNote(const QString &dir, const QJsonObject &note)
{
    QFile file(AthleteLock::notePath(dir));
    file.open(QFile::WriteOnly);
    file.write(QJsonDocument(note).toJson());
}

static QJsonObject readNote(const QString &dir)
{
    QFile file(AthleteLock::notePath(dir));
    if (!file.open(QFile::ReadOnly)) return QJsonObject();
    return QJsonDocument::fromJson(file.readAll()).object();
}

class TestAthleteLock : public QObject
{
    Q_OBJECT

    QProcess *startHolder(const QString &dir) {
        QProcess *p = new QProcess(this);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("HOLD_LOCK", dir);
        p->setProcessEnvironment(env);
        p->start(QCoreApplication::applicationFilePath(), QStringList());
        p->waitForReadyRead(5000);
        return p;
    }

private slots:

    void lockAndUnlock() {
        QTemporaryDir dir;
        AthleteLock lock(dir.path());
        QVERIFY(!AthleteLock::heldByThisProcess(dir.path()));
        QVERIFY(lock.tryLock());
        QVERIFY(lock.isLocked());
        QVERIFY(QFile::exists(AthleteLock::lockFilePath(dir.path())));
        QVERIFY(AthleteLock::heldByThisProcess(dir.path()));
        lock.unlock();
        QVERIFY(!AthleteLock::heldByThisProcess(dir.path()));
        QVERIFY(!QFile::exists(AthleteLock::lockFilePath(dir.path())));
    }

    void lockIsNotInTheAthleteFolder() {
        QTemporaryDir dir;
        QString path = AthleteLock::lockFilePath(dir.path());
        QVERIFY(!path.startsWith(QDir(dir.path()).canonicalPath()));
        QVERIFY(path.startsWith(QDir(qEnvironmentVariable("XDG_RUNTIME_DIR")).canonicalPath()));
        QCOMPARE(AthleteLock::lockFilePath(dir.path() + "/./"), path);
        QTemporaryDir other;
        QVERIFY(AthleteLock::lockFilePath(other.path()) != path);
    }

    void sharedWithinProcessInAnyOrder() {
        QTemporaryDir dir;
        AthleteLock *first = new AthleteLock(dir.path());
        AthleteLock *second = new AthleteLock(dir.path() + "/./");  // same folder, other spelling
        QVERIFY(first->tryLock());
        QVERIFY(second->tryLock());

        // the first holder going away must not release the second one's lock
        delete first;
        QVERIFY(AthleteLock::heldByThisProcess(dir.path()));
        QVERIFY(QFile::exists(AthleteLock::lockFilePath(dir.path())));
        delete second;
        QVERIFY(!QFile::exists(AthleteLock::lockFilePath(dir.path())));
    }

    void destructorReleases() {
        QTemporaryDir dir;
        {
            AthleteLock lock(dir.path());
            QVERIFY(lock.tryLock());
        }
        QVERIFY(!AthleteLock::heldByThisProcess(dir.path()));
    }

    void anotherProcessHoldsIt() {
        QTemporaryDir dir;
        QProcess *holder = startHolder(dir.path());
        QCOMPARE(QString(holder->readLine()).trimmed(), QString("locked"));

        AthleteLock lock(dir.path());
        QVERIFY(!lock.tryLock(0));
        QCOMPARE(lock.holderPid(), qint64(holder->processId()));
        QVERIFY(lock.holder().contains(QString::number(holder->processId())));

        // waiting gives up after the timeout
        QElapsedTimer t;
        t.start();
        QVERIFY(!lock.tryLock(300));
        QVERIFY(t.elapsed() >= 250);

        holder->kill();
        holder->waitForFinished();
    }

    void peekLeavesTheLockAlone() {
        QTemporaryDir dir;
        QCOMPARE(AthleteLock::peek(dir.path()), AthleteLock::State::Free);

        QProcess *holder = startHolder(dir.path());
        QCOMPARE(QString(holder->readLine()).trimmed(), QString("locked"));
        QString who;
        QCOMPARE(AthleteLock::peek(dir.path(), &who), AthleteLock::State::InUse);
        QVERIFY(who.contains(QString::number(holder->processId())));
        QVERIFY(QFile::exists(AthleteLock::lockFilePath(dir.path())));
        QVERIFY(!AthleteLock::heldByThisProcess(dir.path()));

        // killed without unlocking: stale, so free, and the file is left
        // for the next tryLock to remove
        holder->kill();
        holder->waitForFinished();
        QCOMPARE(AthleteLock::peek(dir.path()), AthleteLock::State::Free);
        QVERIFY(QFile::exists(AthleteLock::lockFilePath(dir.path())));

        // this process's own lock is no obstacle
        AthleteLock lock(dir.path());
        QVERIFY(lock.tryLock());
        QCOMPARE(AthleteLock::peek(dir.path()), AthleteLock::State::Free);
    }

    void staleLockOfDeadProcessIsTakenOver() {
        QTemporaryDir dir;
        QProcess *holder = startHolder(dir.path());
        QCOMPARE(QString(holder->readLine()).trimmed(), QString("locked"));

        // killed without unlocking: the lock file stays behind
        holder->kill();
        holder->waitForFinished();
        QVERIFY(QFile::exists(AthleteLock::lockFilePath(dir.path())));

        AthleteLock lock(dir.path());
        QVERIFY(lock.tryLock(0));
    }

    void noteInTheAthleteFolderWhileLocked() {
        QTemporaryDir dir;
        AthleteLock::setHolderName("Lock test");
        AthleteLock lock(dir.path());
        QVERIFY(lock.tryLock());
        QJsonObject note = readNote(dir.path());
        QCOMPARE(note.value("app").toString(), QString("Lock test"));
        QCOMPARE(qint64(note.value("pid").toDouble()), QCoreApplication::applicationPid());
        QCOMPARE(note.value("system").toString(), AthleteLock::systemId());
        QVERIFY(AthleteLock::notePath(dir.path()).startsWith(QDir(dir.path()).canonicalPath()));
        lock.unlock();
        QVERIFY(!QFile::exists(AthleteLock::notePath(dir.path())));
        AthleteLock::setHolderName(QString());
    }

    void noteFromAnotherSystemStopsTheLock() {
        // WSL and Windows, a container and its host: they can't see each
        // other's lock file, only the note
        QTemporaryDir dir;
        writeNote(dir.path(), QJsonObject{ { "app", "GoldenCheetah" }, { "pid", 42 }, { "host", "elsewhere" },
                                           { "os", "Windows 11" }, { "system", "another-system" } });
        AthleteLock lock(dir.path());
        QVERIFY(!lock.tryLock(0));
        QVERIFY(lock.heldElsewhere());
        QCOMPARE(lock.holder(), QString("GoldenCheetah (pid 42 on elsewhere, Windows 11)"));
        QString who;
        QCOMPARE(AthleteLock::peek(dir.path(), &who), AthleteLock::State::InUse);
        QCOMPARE(who, lock.holder());

        // it can't be checked, so it waits as long as asked to
        QElapsedTimer t;
        t.start();
        QVERIFY(!lock.tryLock(300));
        QVERIFY(t.elapsed() >= 250);

        // forced: the note is ours, and goes with the lock
        QVERIFY(lock.tryLock(0, true));
        QCOMPARE(readNote(dir.path()).value("system").toString(), AthleteLock::systemId());
        lock.unlock();
        QVERIFY(!QFile::exists(AthleteLock::notePath(dir.path())));
    }

    void staleNoteOfThisSystemIsReplaced() {
        QTemporaryDir dir;
        QProcess gone;
        gone.start(QCoreApplication::applicationFilePath(), QStringList() << "-functions");
        gone.waitForFinished();
        writeNote(dir.path(), QJsonObject{ { "app", "GoldenCheetah" }, { "pid", double(gone.processId()) },
                                           { "system", AthleteLock::systemId() } });
        QCOMPARE(AthleteLock::peek(dir.path()), AthleteLock::State::Free);
        AthleteLock lock(dir.path());
        QVERIFY(lock.tryLock(0));
        QCOMPARE(qint64(readNote(dir.path()).value("pid").toDouble()), QCoreApplication::applicationPid());
    }

    void unreadableNoteIsIgnored() {
        QTemporaryDir dir;
        QFile file(AthleteLock::notePath(dir.path()));
        file.open(QFile::WriteOnly);
        file.write("{ half written");
        file.close();
        AthleteLock lock(dir.path());
        QVERIFY(lock.tryLock(0));
    }
};

int main(int argc, char **argv)
{
    QByteArray hold = qgetenv("HOLD_LOCK");
    if (!hold.isEmpty()) return holdLock(QString::fromLocal8Bit(hold));

    // locks go in the runtime folder, one of our own (the holder inherits it)
    QTemporaryDir runtime;
    qputenv("XDG_RUNTIME_DIR", QDir(runtime.path()).canonicalPath().toLocal8Bit());

    QCoreApplication app(argc, argv);
    TestAthleteLock test;
    return QTest::qExec(&test, argc, argv);
}

#include "testAthleteLock.moc"
