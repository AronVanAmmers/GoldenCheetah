#include "ResultFormat.h"

#include <QTest>

using namespace Headless;

class TestResultFormat : public QObject
{
    Q_OBJECT

private slots:

    void envelope() {
        CommandResult r = CommandResult::failure(Status::NotFound, "gone");
        QJsonObject e = ResultFormat::envelope("activity.show", r);
        QCOMPARE(e.value("ok").toBool(), false);
        QCOMPARE(e.value("status").toString(), QString("not_found"));
        QCOMPARE(e.value("command").toString(), QString("activity.show"));
        QCOMPARE(e.value("error").toString(), QString("gone"));

        CommandResult ok = CommandResult::success(QJsonObject{ { "a", 1 } });
        ok.warnings << "careful";
        e = ResultFormat::envelope("x", ok);
        QCOMPARE(e.value("ok").toBool(), true);
        QVERIFY(!e.contains("error"));
        QCOMPARE(e.value("warnings").toArray().count(), 1);
    }

    void scalars() {
        QCOMPARE(ResultFormat::scalar(QJsonValue(3.0)), QString("3"));
        QCOMPARE(ResultFormat::scalar(QJsonValue(3.14159)), QString("3.142"));
        QCOMPARE(ResultFormat::scalar(QJsonValue(123.456)), QString("123.5"));
        QCOMPARE(ResultFormat::scalar(QJsonValue(0.5)), QString("0.5"));
        QCOMPARE(ResultFormat::scalar(QJsonValue(100.04)), QString("100"));
        QCOMPARE(ResultFormat::scalar(QJsonValue(true)), QString("yes"));
        QCOMPARE(ResultFormat::scalar(QJsonValue()), QString("-"));
        QCOMPARE(ResultFormat::scalar(QJsonArray({ "a", 2 })), QString("a, 2"));
    }

    void tableAlignsAndFlattens() {
        QJsonArray rows;
        rows.append(QJsonObject{ { "id", "a" }, { "start", "2024-01-01" }, { "metrics", QJsonObject{ { "tss", 50 } } } });
        rows.append(QJsonObject{ { "id", "bbbb" }, { "start", "2024-01-02" }, { "metrics", QJsonObject{ { "tss", 7.5 } } } });
        QString t = ResultFormat::table(rows);
        QStringList lines = t.split('\n', Qt::SkipEmptyParts);
        QCOMPARE(lines.count(), 3);
        QVERIFY(lines[0].startsWith("id    start"));   // preferred columns first, aligned
        QVERIFY(lines[0].contains("tss"));           // nested metrics become columns
        QVERIFY(lines[2].startsWith("bbbb  2024-01-02"));
        QVERIFY(!lines[0].endsWith(" "));            // no trailing blanks
        QCOMPARE(ResultFormat::table(QJsonArray()), QString("(none)\n"));
    }

    void tableSkipsLongTextColumns() {
        QJsonArray rows{ QJsonObject{ { "name", "x" }, { "type", "python" }, { "description", "long words" } } };
        QVERIFY(!ResultFormat::table(rows).contains("description"));
    }

    void renderSingleListAsTable() {
        QJsonObject data{ { "count", 1 }, { "activities", QJsonArray{ QJsonObject{ { "id", "a" } } } } };
        QString t = ResultFormat::render(data);
        QVERIFY(t.startsWith("id\na\n"));
        QVERIFY(t.contains("count: 1"));
    }

    void renderNested() {
        QJsonObject data{ { "name", "Joe" }, { "sports", QJsonObject{ { "Bike", 3 } } } };
        QString t = ResultFormat::render(data);
        QVERIFY(t.contains("name:"));
        QVERIFY(t.contains("sports:\n  Bike: 3"));
    }

    void handlerTextWins() {
        CommandResult r = CommandResult::success(QJsonObject{ { "a", 1 } });
        r.text = "custom\n";
        QCOMPARE(ResultFormat::text(r), QString("custom\n"));
    }
};

QTEST_MAIN(TestResultFormat)
#include "testResultFormat.moc"
