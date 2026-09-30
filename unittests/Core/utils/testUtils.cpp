#include "Core/Utils.h"

#include <QTest>


class TestUtils: public QObject
{
    Q_OBJECT

private slots:
    void quoteEscapeTest() {
        QCOMPARE(Utils::quoteEscape("abc"), QString("abc"));
        QCOMPARE(Utils::quoteEscape("a\bc"), QString("a\bc"));
        QCOMPARE(Utils::quoteEscape("a\\bc"), QString("a\\bc"));
        QCOMPARE(Utils::quoteEscape("a\"bc"), QString("a\\\"bc"));
        QCOMPARE(Utils::quoteEscape("a\\\"bc"), QString("a\\\"bc"));
        QCOMPARE(Utils::quoteEscape("a\\\\\"bc"), QString("a\\\\\\\"bc"));
    }

    void argsortShownTest() {
        // a table column sorts as its values read: dates across months as dates
        QCOMPARE(Utils::argsortShown({ "28 Feb 2026", "02 Mar 2026", "15 Jan 2026" }, true), QVector<int>({ 2, 0, 1 }));
        QCOMPARE(Utils::argsortShown({ "1:05:00", "59:30", "2:00:00" }, true), QVector<int>({ 1, 0, 2 }));
        QCOMPARE(Utils::argsortShown({ "10", "9.5", "-1" }, false), QVector<int>({ 0, 1, 2 }));
        QCOMPARE(Utils::argsortShown({ "b", "a", "12" }, true), QVector<int>({ 2, 1, 0 }));
    }
};


QTEST_MAIN(TestUtils)
#include "testUtils.moc"
