#include "SeasonDefinition.h"

#include <QTest>
#include <QJsonObject>

using namespace Headless;

class TestSeasonDefinition : public QObject
{
    Q_OBJECT

    // a season made from the flags, as season add does
    static bool make(const QJsonObject &args, Season &season, QString &error, int type = Season::season) {
        SeasonDefinition def;
        if (!def.applyArgs(args, error)) return false;
        error = def.check(type, false);
        if (!error.isEmpty()) return false;
        season.setType(type);
        def.applyTo(season);
        return true;
    }

private slots:

    void lengths() {
        SeasonLength l;
        QString error;
        QVERIFY(parseSeasonLength("1y2m3d", l, error));
        QCOMPARE(l, SeasonLength(1, 2, 3));
        QVERIFY(parseSeasonLength("6m", l, error));
        QCOMPARE(l, SeasonLength(0, 6, 0));
        QVERIFY(parseSeasonLength("1Y 10D", l, error));
        QCOMPARE(l, SeasonLength(1, 0, 10));
        QCOMPARE(seasonLengthText(SeasonLength(1, 0, 10)), QString("1y10d"));
        QCOMPARE(seasonLengthText(SeasonLength(0, 0, 0)), QString("0d"));
        for (const char *bad : { "", "1x", "m", "2d1y", "13m", "51y", "32d", "-1d" }) {
            error.clear();
            QVERIFY2(!parseSeasonLength(bad, l, error), bad);
            QVERIFY(!error.isEmpty());
        }
    }

    void absolute() {
        Season s;
        QString error;
        QVERIFY(make({ { "from", "2026-01-01" }, { "to", "2026-12-31" } }, s, error));
        QCOMPARE(s.getAbsoluteStart(), QDate(2026, 1, 1));
        QCOMPARE(s.getAbsoluteEnd(), QDate(2026, 12, 31));
        QVERIFY(s.isAbsolute());
        QVERIFY(s.canHavePhasesOrEvents());
    }

    void relativeAsTheDialog() {
        // "3 months ago", not aligned, as EditSeasonDialog::getSeasonOffset makes it
        Season s;
        QString error;
        QVERIFY(make({ { "start-ago", 3 }, { "start-unit", "months" }, { "length", "2m" } }, s, error));
        QVERIFY(s.getOffsetStart() == SeasonOffset(1, -3, 1, false));
        QCOMPARE(s.getLength(), SeasonLength(0, 2, 0));
        QDate today(2026, 10, 2);
        QCOMPARE(s.getStart(today), QDate(2026, 7, 2));
        QCOMPARE(s.getEnd(today), QDate(2026, 9, 1));
        QVERIFY(!s.isAbsolute());

        // weeks by default
        Season w;
        QVERIFY(make({ { "from", "2026-01-01" }, { "end-ago", 2 } }, w, error));
        QVERIFY(w.getOffsetEnd() == SeasonOffset(1, 1, -2, false));
        QCOMPARE(w.getEnd(today), today.addDays(-14));
    }

    void lengthBeforeTheEnd() {
        Season s;
        QString error;
        QVERIFY(make({ { "to", "2026-06-30" }, { "length", "3m" } }, s, error));
        QCOMPARE(s.getAbsoluteEnd(), QDate(2026, 6, 30));
        QCOMPARE(s.getStart(), QDate(2026, 3, 31));
        QCOMPARE(SeasonDefinition::of(s).start.kind, SeasonBound::Duration);
        QCOMPARE(SeasonDefinition::of(s).end.kind, SeasonBound::Absolute);
        QVERIFY(s.isAbsolute());
    }

    void yearToDate() {
        Season s;
        QString error;
        QVERIFY(make({ { "from", "2025-01-01" }, { "ytd", true } }, s, error));
        QVERIFY(s.isYtd());
        QCOMPARE(s.getEnd(QDate(2026, 10, 2)), QDate(2025, 10, 2));
    }

    void refusals() {
        Season s;
        QString error;
        QVERIFY(!make({ { "from", "2026-01-01" } }, s, error));
        QVERIFY(error.contains("end"));
        QVERIFY(!make({ { "to", "2026-01-01" } }, s, error));
        QVERIFY(error.contains("start"));
        QVERIFY(!make({ { "from", "2026-01-01" }, { "start-ago", 2 }, { "to", "2026-02-01" } }, s, error));
        QVERIFY(!make({ { "from", "2026-01-01" }, { "to", "2026-02-01" }, { "ytd", true } }, s, error));
        QVERIFY(!make({ { "from", "2026-01-01" }, { "to", "2026-02-01" }, { "length", "1m" } }, s, error));
        QVERIFY(!make({ { "length", "1m" } }, s, error));
        QVERIFY(!make({ { "length", "1m" }, { "ytd", true } }, s, error));
        QVERIFY(!make({ { "from", "2026-01-01" }, { "to", "2026-02-01" }, { "start-unit", "weeks" } }, s, error));
        QVERIFY(!make({ { "start-ago", 53 }, { "to", "2026-02-01" } }, s, error));
        // a cycle or adhoc range has fixed dates
        QVERIFY(!make({ { "start-ago", 2 }, { "to", "2026-02-01" } }, s, error, Season::cycle));
        QVERIFY(!make({ { "from", "2026-01-01" }, { "length", "1m" } }, s, error, Season::adhoc));
        QVERIFY(make({ { "from", "2026-01-01" }, { "to", "2026-02-01" } }, s, error, Season::adhoc));

        // with phases or events it can't move with today
        SeasonDefinition def;
        QVERIFY(def.applyArgs({ { "start-ago", 2 }, { "to", "2026-02-01" } }, error));
        QVERIFY(def.check(Season::season, false).isEmpty());
        QVERIFY(!def.check(Season::season, true).isEmpty());
        QVERIFY(def.applyArgs({ { "from", "2026-01-01" }, { "length", "1m" } }, error));
        QVERIFY(def.check(Season::season, true).isEmpty());
    }

    void editingOneSide() {
        Season s;
        QString error;
        QVERIFY(make({ { "from", "2026-01-01" }, { "to", "2026-12-31" } }, s, error));

        // --length alone replaces the end
        SeasonDefinition def = SeasonDefinition::of(s);
        QVERIFY(def.applyArgs({ { "length", "6m" } }, error));
        def.applyTo(s);
        QCOMPARE(s.getStart(), QDate(2026, 1, 1));
        QCOMPARE(s.getEnd(), QDate(2026, 6, 30));

        // a new end keeps the start
        def = SeasonDefinition::of(s);
        QVERIFY(def.applyArgs({ { "to", "2026-03-31" } }, error));
        def.applyTo(s);
        QCOMPARE(s.getAbsoluteStart(), QDate(2026, 1, 1));
        QCOMPARE(s.getAbsoluteEnd(), QDate(2026, 3, 31));
        QVERIFY(!s.getLength().isValid());

        // a length before the end stays one when only the length changes
        QVERIFY(make({ { "to", "2026-06-30" }, { "length", "3m" } }, s, error));
        def = SeasonDefinition::of(s);
        QVERIFY(def.applyArgs({ { "length", "1m" } }, error));
        def.applyTo(s);
        QCOMPARE(s.getAbsoluteEnd(), QDate(2026, 6, 30));
        QCOMPARE(s.getStart(), QDate(2026, 5, 31));
    }

    void definitionsReadBack() {
        // of() reads back what applyTo() wrote, for every kind
        const QList<QJsonObject> cases = {
            { { "from", "2026-01-01" }, { "to", "2026-02-01" } },
            { { "start-ago", 4 }, { "start-unit", "years" }, { "end-ago", 1 }, { "end-unit", "months" } },
            { { "start-ago", 4 }, { "length", "1y2m3d" } },
            { { "end-ago", 0 }, { "length", "10d" } },
            { { "start-ago", 1 }, { "start-unit", "years" }, { "ytd", true } },
        };
        for (const QJsonObject &args : cases) {
            SeasonDefinition def;
            QString error;
            QVERIFY(def.applyArgs(args, error));
            Season s;
            def.applyTo(s);
            SeasonDefinition back = SeasonDefinition::of(s);
            QCOMPARE(back.start.kind, def.start.kind);
            QCOMPARE(back.end.kind, def.end.kind);
            QCOMPARE(back.json(), def.json());
        }
    }

    void names() {
        int type = 0;
        QVERIFY(parsePhaseType("Build", type));
        QCOMPARE(type, int(Phase::build));
        QVERIFY(!parsePhaseType("peak", type)); // the dialog doesn't offer it
        QCOMPARE(phaseTypeName(Phase::peak), QString("peak"));
        QCOMPARE(seasonTypeName(Season::temporary), QString("system"));

        int priority = -1;
        QVERIFY(parseEventPriority("b", priority));
        QCOMPARE(priority, 2);
        QVERIFY(parseEventPriority("none", priority));
        QCOMPARE(priority, 0);
        QVERIFY(!parseEventPriority("F", priority));
        QCOMPARE(eventPriorityName(5), QString("E"));
        QCOMPARE(eventPriorityName(0), QString());
    }
};

QTEST_MAIN(TestSeasonDefinition)
#include "testSeasonDefinition.moc"
