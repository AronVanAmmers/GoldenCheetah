#include "testCommands.h"

#include <QTest>
#include <QDir>

class TestCommandRegistry : public QObject
{
    Q_OBJECT

private slots:

    void findAndList() {
        CommandRegistry r = testRegistry();
        QVERIFY(r.find("import"));
        QVERIFY(!r.find("nope"));
        QCOMPARE(r.names().first(), QString("activity.delete")); // sorted
        QCOMPARE(r.namesWithPrefix("activity"), QStringList({ "activity.delete", "activity.list", "activity.show" }));
        QCOMPARE(r.namesWithPrefix("cp"), QStringList({ "cp", "cp.estimates" }));
        QVERIFY(r.namesWithPrefix("act").isEmpty()); // whole words only
    }

    void coerceStringsFromArgv() {
        QString error;
        ParamSpec i("n", ParamType::Int, "");
        QCOMPARE(CommandRegistry::coerce(i, QJsonValue("42"), error).toInt(), 42);
        QVERIFY(error.isEmpty());
        CommandRegistry::coerce(i, QJsonValue("4.2"), error);
        QVERIFY(!error.isEmpty());
        CommandRegistry::coerce(i, QJsonValue(4.5), error);
        QVERIFY(!error.isEmpty());

        ParamSpec d("x", ParamType::Double, "");
        QCOMPARE(CommandRegistry::coerce(d, QJsonValue("3.25"), error).toDouble(), 3.25);
        CommandRegistry::coerce(d, QJsonValue("abc"), error);
        QVERIFY(error.contains("abc"));
        error.clear();
        CommandRegistry::coerce(d, QJsonValue("nan"), error);
        QVERIFY(error.contains("nan"));
        error.clear();
        CommandRegistry::coerce(d, QJsonValue("-inf"), error);
        QVERIFY(error.contains("inf"));

        ParamSpec b("f", ParamType::Bool, "");
        QCOMPARE(CommandRegistry::coerce(b, QJsonValue("yes"), error).toBool(), true);
        QCOMPARE(CommandRegistry::coerce(b, QJsonValue("0"), error).toBool(), false);
        QCOMPARE(CommandRegistry::coerce(b, QJsonValue(true), error).toBool(), true);
        CommandRegistry::coerce(b, QJsonValue("maybe"), error);
        QVERIFY(!error.isEmpty());

        ParamSpec date("d", ParamType::Date, "");
        QCOMPARE(CommandRegistry::coerce(date, QJsonValue("2024-02-29"), error).toString(), QString("2024-02-29"));
        CommandRegistry::coerce(date, QJsonValue("2023-02-29"), error);
        QVERIFY(!error.isEmpty());
    }

    void pathsBecomeAbsolute() {
        QString error;
        ParamSpec p("file", ParamType::Path, "");
        QString v = CommandRegistry::coerce(p, QJsonValue("some/file.fit"), error).toString();
        QVERIFY(error.isEmpty());
        QCOMPARE(v, QDir::cleanPath(QDir::current().absoluteFilePath("some/file.fit")));
        QCOMPARE(CommandRegistry::coerce(p, QJsonValue("-"), error).toString(), QString("-"));
    }

    void choicesAreCaseInsensitiveAndCanonical() {
        QString error;
        ParamSpec p("as", ParamType::String, "");
        p.oneOf({ "png", "svg" });
        QCOMPARE(CommandRegistry::coerce(p, QJsonValue("SVG"), error).toString(), QString("svg"));
        CommandRegistry::coerce(p, QJsonValue("gif"), error);
        QVERIFY(error.contains("png, svg"));
    }

    void validateAppliesDefaultsAndRequired() {
        CommandRegistry r = testRegistry();
        const CommandSpec &chart = r.find("chart.activity")->spec;

        QJsonObject args{ { "activity", "last" } };
        QCOMPARE(CommandRegistry::validate(chart, args), QString());
        QCOMPARE(args.value("as").toString(), QString("png"));
        QCOMPARE(args.value("width").toInt(), 1200);
        QVERIFY(!args.contains("smooth"));

        QJsonObject missing;
        QVERIFY(CommandRegistry::validate(chart, missing).contains("missing required parameter 'activity'"));
    }

    void validateRejectsUnknownParameters() {
        CommandRegistry r = testRegistry();
        QJsonObject args{ { "activity", "last" }, { "colour", "red" } };
        QString error = CommandRegistry::validate(r.find("chart.activity")->spec, args);
        QVERIFY(error.contains("unknown parameter 'colour'"));
    }

    void validateRepeatedAndSingle() {
        CommandRegistry r = testRegistry();
        const CommandSpec &list = r.find("activity.list")->spec;

        // a single value for a repeated parameter becomes a list
        QJsonObject a{ { "metric", "average_power" } };
        QCOMPARE(CommandRegistry::validate(list, a), QString());
        QCOMPARE(a.value("metric").toArray().count(), 1);

        // a list of one for a single parameter is unwrapped, more is an error
        QJsonObject b{ { "limit", QJsonArray{ "5" } } };
        QCOMPARE(CommandRegistry::validate(list, b), QString());
        QCOMPARE(b.value("limit").toInt(), 5);
        QJsonObject c{ { "limit", QJsonArray{ "5", "6" } } };
        QVERIFY(CommandRegistry::validate(list, c).contains("only be given once"));

        // required repeated parameters need at least one value
        QJsonObject d{ { "file", QJsonArray() } };
        QVERIFY(!CommandRegistry::validate(r.find("import")->spec, d).isEmpty());
    }

    void describeIsMachineReadable() {
        CommandRegistry r = testRegistry();
        QJsonObject o = CommandRegistry::describe(r.find("chart.activity")->spec);
        QCOMPARE(o.value("cli").toString(), QString("chart activity"));
        QCOMPARE(o.value("method").toString(), QString("GET"));
        QJsonArray params = o.value("params").toArray();
        QCOMPARE(params.count(), 4);
        QCOMPARE(params.at(1).toObject().value("choices").toArray().count(), 3);
        QCOMPARE(params.at(1).toObject().value("default").toString(), QString("png"));
    }

    void statusMapping() {
        QCOMPARE(httpStatusFor(Status::Ok), 200);
        QCOMPARE(httpStatusFor(Status::Usage), 400);
        QCOMPARE(httpStatusFor(Status::NotFound), 404);
        QCOMPARE(httpStatusFor(Status::Locked), 409);
        QCOMPARE(statusName(Status::Partial), QString("partial"));
        QCOMPARE(int(Status::Locked), 4); // exit status contract
    }
};

QTEST_MAIN(TestCommandRegistry)
#include "testCommandRegistry.moc"
