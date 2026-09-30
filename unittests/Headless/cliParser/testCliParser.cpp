#include "testCommands.h"
#include "CliParser.h"

#include <QTest>

class TestCliParser : public QObject
{
    Q_OBJECT

    CommandRegistry r = testRegistry();

    CliParse parse(const QStringList &argv) { return CliParser::parse(argv, r); }

private slots:

    void globalOptionsAnywhere() {
        CliParse p = parse({ "--home", "/a", "activity", "list", "--athlete", "Joe", "-f", "json", "-q" });
        QCOMPARE(p.error, QString());
        QCOMPARE(p.command, QString("activity.list"));
        QCOMPARE(p.global.home, QString("/a"));
        QCOMPARE(p.global.athlete, QString("Joe"));
        QCOMPARE(p.global.format, QString("json"));
        QVERIFY(p.global.quiet);
    }

    void equalsSyntax() {
        CliParse p = parse({ "--athlete-dir=/x/Joe", "activity", "list", "--filter=isRun=0", "--lock-wait=5" });
        QCOMPARE(p.error, QString());
        QCOMPARE(p.global.athleteDir, QString("/x/Joe"));
        QCOMPARE(p.global.lockWait, 5);
        QCOMPARE(p.args.value("filter").toString(), QString("isRun=0")); // only the first '=' splits
    }

    void badGlobalValues() {
        QVERIFY(parse({ "--format", "xml", "cp" }).error.contains("'text', 'json' or 'csv'"));
        QCOMPARE(parse({ "-f", "CSV", "cp" }).global.format, QString("csv"));
        QVERIFY(parse({ "--lock-wait", "soon", "cp" }).error.contains("seconds"));
        QVERIFY(parse({ "cp", "--home" }).error.contains("needs a value"));
    }

    void longestCommandMatch() {
        QCOMPARE(parse({ "cp" }).command, QString("cp"));
        QCOMPARE(parse({ "cp", "estimates" }).command, QString("cp.estimates"));
    }

    void positionalsAndRepeats() {
        CliParse p = parse({ "import", "a.fit", "b.fit", "--dry-run", "c.tcx" });
        QCOMPARE(p.error, QString());
        QCOMPARE(p.command, QString("import"));
        QCOMPARE(p.args.value("file").toArray().count(), 3);
        QCOMPARE(p.args.value("dry-run").toString(), QString("true"));
    }

    void flagsNeverSwallowTheNextWord() {
        CliParse p = parse({ "import", "--recursive", "folder" });
        QCOMPARE(p.args.value("recursive").toString(), QString("true"));
        QCOMPARE(p.args.value("file").toString(), QString("folder"));
    }

    void negatedFlags() {
        CliParse p = parse({ "import", "x", "--no-dry-run" });
        QCOMPARE(p.error, QString());
        QCOMPARE(p.args.value("dry-run").toString(), QString("false"));
    }

    void valuedOptionsTakeTheNextWord() {
        CliParse p = parse({ "activity", "list", "--metric", "a", "--metric", "b", "--from", "2024-01-01", "x" });
        QCOMPARE(p.error, QString());
        QCOMPARE(p.args.value("metric").toArray(), QJsonArray({ "a", "b" }));
        QCOMPARE(p.args.value("from").toString(), QString("2024-01-01"));
        QCOMPARE(p.args.value("activity").toString(), QString("x"));
    }

    void negativeNumbersAreValues() {
        CliParse p = parse({ "chart", "activity", "x", "--width", "-5", "--smooth", "-0.5" });
        QCOMPARE(p.error, QString());
        QCOMPARE(p.args.value("width").toString(), QString("-5"));
        QCOMPARE(p.args.value("smooth").toString(), QString("-0.5"));
        QCOMPARE(parse({ "chart", "activity", "x", "--smooth", "-.5" }).args.value("smooth").toString(), QString("-.5"));
        p = parse({ "calendar", "shift", "x", "-3" });
        QCOMPARE(p.error, QString());
        QCOMPARE(p.args.value("days").toString(), QString("-3"));
        QVERIFY(parse({ "activity", "list", "-x" }).error.contains("unknown option"));
    }

    void doubleDashEndsOptions() {
        CliParse p = parse({ "import", "--", "--weird-name.fit" });
        QCOMPARE(p.error, QString());
        QCOMPARE(p.args.value("file").toString(), QString("--weird-name.fit"));
    }

    void errors() {
        QVERIFY(parse({ "fly" }).error.contains("unknown command 'fly'"));
        QVERIFY(parse({ "activity" }).error.contains("needs a subcommand"));
        QCOMPARE(parse({ "activity" }).helpTopic, QString("activity"));
        QVERIFY(parse({ "activity", "show", "a", "b" }).error.contains("unexpected argument 'b'"));
        QVERIFY(parse({ "activity", "list", "--colour", "red" }).error.contains("unknown option '--colour'"));
        QVERIFY(parse({ "activity", "list", "--filter" }).error.contains("needs a value"));
        QVERIFY(parse({ "--filter", "x", "activity", "list" }).error.contains("unknown option"));
    }

    void help() {
        CliParse p = parse({ "--help" });
        QVERIFY(p.help);
        QVERIFY(p.command.isEmpty());

        p = parse({ "help", "activity", "show" });
        QVERIFY(p.help);
        QCOMPARE(p.helpTopic, QString("activity.show"));

        p = parse({ "activity", "--help" });
        QVERIFY(p.help);
        QCOMPARE(p.helpTopic, QString("activity"));
        QVERIFY(p.error.isEmpty());

        QVERIFY(parse({ "--version" }).version);
    }

    void helpTexts() {
        QString usage = CliParser::usage(r, "gc");
        QVERIFY(usage.contains("activity list"));
        QVERIFY(usage.contains("Exit status"));

        QString one = CliParser::commandHelp(r.find("chart.activity")->spec, "gc");
        QVERIFY(one.contains("Usage: gc chart activity ACTIVITY [--as png|svg|pdf] [--width INT]"));
        QVERIFY(one.contains("one of: png, svg, pdf"));
        QVERIFY(one.contains("default: png"));
        QVERIFY(one.contains("REST: GET /v1/athletes/{athlete}/activities/{activity}/chart"));

        QString group = CliParser::groupHelp(r, "activity", "gc");
        QVERIFY(group.contains("activity show"));
        QVERIFY(!group.contains("import"));
    }

    void parsedArgsValidate() {
        CliParse p = parse({ "chart", "activity", "last", "--width", "800", "--as", "SVG" });
        QJsonObject args = p.args;
        QCOMPARE(CommandRegistry::validate(r.find(p.command)->spec, args), QString());
        QCOMPARE(args.value("width").toInt(), 800);
        QCOMPARE(args.value("as").toString(), QString("svg"));
    }
};

QTEST_MAIN(TestCliParser)
#include "testCliParser.moc"
