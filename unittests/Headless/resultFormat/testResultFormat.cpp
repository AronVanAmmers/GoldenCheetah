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

    void csvQuotesOnlyWhereNeeded() {
        QCOMPARE(ResultFormat::csvLine({ "a", "b c", "" }), QString("a,b c,\n"));
        QCOMPARE(ResultFormat::csvLine({ "x,y", "say \"hi\"", "two\nlines", " pad" }),
                 QString("\"x,y\",\"say \"\"hi\"\"\",\"two\nlines\",\" pad\"\n"));
        QCOMPARE(ResultFormat::csvValue(QJsonValue(159.82194)), QString("159.82194"));   // full precision
        QCOMPARE(ResultFormat::csvValue(QJsonValue(3.0)), QString("3"));
        QCOMPARE(ResultFormat::csvValue(QJsonValue()), QString());
        QCOMPARE(ResultFormat::csvValue(QJsonValue(false)), QString("false"));
    }

    void csvSingleListIsTheTable() {
        QJsonArray rows;
        rows.append(QJsonObject{ { "name", "Lap 1" }, { "number", 2 }, { "metrics", QJsonObject{ { "average_power", 221.5 } } } });
        rows.append(QJsonObject{ { "name", "Lap 2" }, { "number", 3 }, { "metrics", QJsonObject{ { "average_power", 112.9 } } } });
        QString t = ResultFormat::csv(QJsonObject{ { "activity", "x" }, { "intervals", rows } });
        QCOMPARE(t, QString("number,name,average_power\n2,Lap 1,221.5\n3,Lap 2,112.9\n"));

        // a nested name that clashes with a column keeps its object's name
        QJsonArray clash{ QJsonObject{ { "name", "a" }, { "metadata", QJsonObject{ { "name", "b" } } } } };
        QCOMPARE(ResultFormat::csv(QJsonObject{ { "list", clash } }), QString("name,metadata.name\na,b\n"));
    }

    void csvAnythingElseIsKeyValue() {
        QJsonObject data{ { "name", "Joe" }, { "metrics", QJsonObject{ { "tss", 50 } } },
                          { "intervals", QJsonArray{ QJsonObject{ { "name", "Lap 1" } } } }, { "tags", QJsonArray{ "a", "b" } } };
        QCOMPARE(ResultFormat::csv(data), QString("key,value\nintervals[0].name,Lap 1\nmetrics.tss,50\nname,Joe\ntags,a; b\n"));

        CommandResult r = CommandResult::success(data);
        r.csv = "own\n";
        QCOMPARE(ResultFormat::csv(r), QString("own\n"));
    }

    void handlerTextWins() {
        CommandResult r = CommandResult::success(QJsonObject{ { "a", 1 } });
        r.text = "custom\n";
        QCOMPARE(ResultFormat::text(r), QString("custom\n"));
    }

    void statusLines() {
        QJsonObject done{ { "status", "imported" }, { "source", "a.fit" } };
        QCOMPARE(ResultFormat::statusLine(done, "source", 8, "  -> x"), QString("imported  a.fit  -> x\n"));
        QJsonObject failed{ { "status", "failed" }, { "name", "F" }, { "message", "why" } };
        QCOMPARE(ResultFormat::statusLine(failed, "name", 9), QString("failed     F  why\n"));
    }

    void batchStatus() {
        QCOMPARE(CommandResult::batch(QJsonObject(), 0, 3, "e").status, Status::Ok);
        CommandResult some = CommandResult::batch(QJsonObject(), 1, 3, "1 failed");
        QCOMPARE(some.status, Status::Partial);
        QCOMPARE(some.error, QString("1 failed"));
        QCOMPARE(CommandResult::batch(QJsonObject(), 3, 3, "e").status, Status::Failed);
    }
};

QTEST_MAIN(TestResultFormat)
#include "testResultFormat.moc"
