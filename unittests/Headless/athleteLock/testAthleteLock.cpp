#include "AthleteLock.h"

#include <QTest>
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QFile>
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
};

int main(int argc, char **argv)
{
    QByteArray hold = qgetenv("HOLD_LOCK");
    if (!hold.isEmpty()) return holdLock(QString::fromLocal8Bit(hold));

    QCoreApplication app(argc, argv);
    TestAthleteLock test;
    return QTest::qExec(&test, argc, argv);
}

#include "testAthleteLock.moc"
