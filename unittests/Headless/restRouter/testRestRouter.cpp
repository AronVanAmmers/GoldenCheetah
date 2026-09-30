#include "testCommands.h"
#include "RestRouter.h"

#include <QTest>

class TestRestRouter : public QObject
{
    Q_OBJECT

    CommandRegistry r = testRegistry();

    RestRouter::Match go(const QString &method, const QString &path,
                         const QMultiMap<QString,QString> &q = {}, const QByteArray &body = QByteArray()) {
        RestRouter router(r);
        return router.match(method, path, q, body);
    }

private slots:

    void pathParameters() {
        RestRouter::Match m = go("GET", "/v1/athletes/Joe/activities/2024_01_01_10_00_00");
        QCOMPARE(m.httpStatus, 200);
        QCOMPARE(m.command, QString("activity.show"));
        QCOMPARE(m.athlete, QString("Joe"));
        QCOMPARE(m.args.value("activity").toString(), QString("2024_01_01_10_00_00"));
    }

    void repeatedPathParameterBecomesList() {
        RestRouter::Match m = go("DELETE", "/v1/athletes/Joe/activities/last");
        QCOMPARE(m.command, QString("activity.delete"));
        QCOMPARE(m.args.value("activity").toArray(), QJsonArray({ "last" }));
    }

    void queryParameters() {
        QMultiMap<QString,QString> q;
        q.insert("filter", "isRun=0");
        q.insert("metric", "a");
        q.insert("metric", "b");
        RestRouter::Match m = go("GET", "/v1/athletes/Joe/activities", q);
        QCOMPARE(m.command, QString("activity.list"));
        QCOMPARE(m.args.value("filter").toString(), QString("isRun=0"));
        QCOMPARE(m.args.value("metric").toArray().count(), 2);
    }

    void jsonBodyIsTyped() {
        RestRouter::Match m = go("POST", "/v1/athletes/Joe/imports", {}, "{\"file\":[\"/x.fit\"],\"dry-run\":true}");
        QCOMPARE(m.httpStatus, 200);
        QCOMPARE(m.command, QString("import"));
        QVERIFY(m.args.value("dry-run").isBool());
        QCOMPARE(m.args.value("file").toArray().count(), 1);
    }

    void badBody() {
        RestRouter::Match m = go("POST", "/v1/athletes/Joe/imports", {}, "[1,2]");
        QCOMPARE(m.httpStatus, 400);
    }

    void literalSegmentsWin() {
        QCOMPARE(go("GET", "/v1/athletes/Joe/cp/estimates").command, QString("cp.estimates"));
        QCOMPARE(go("GET", "/v1/athletes/Joe/cp").command, QString("cp"));
        QCOMPARE(go("GET", "/v1/athletes/Joe/activities/last/chart").command, QString("chart.activity"));
    }

    void notFoundAndMethodNotAllowed() {
        QCOMPARE(go("GET", "/v1/nothing").httpStatus, 404);
        QCOMPARE(go("GET", "/api/athletes").httpStatus, 404);
        RestRouter::Match m = go("PUT", "/v1/athletes/Joe/activities/x");
        QCOMPARE(m.httpStatus, 405);
        QVERIFY(m.allowed.contains("GET"));
        QVERIFY(m.allowed.contains("DELETE"));
    }

    void genericCommandRoute() {
        RestRouter::Match m = go("POST", "/v1/commands/activity.list", {},
                                 "{\"athlete\":\"Joe\",\"args\":{\"limit\":3}}");
        QCOMPARE(m.httpStatus, 200);
        QCOMPARE(m.command, QString("activity.list"));
        QCOMPARE(m.athlete, QString("Joe"));
        QCOMPARE(m.args.value("limit").toInt(), 3);
        QCOMPARE(go("GET", "/v1/commands/activity.list").httpStatus, 405);
        QCOMPARE(go("POST", "/v1/commands/nope").httpStatus, 404);
    }

    void globalRouteWithAthleteQuery() {
        QMultiMap<QString,QString> q;
        q.insert("athlete", "Joe");
        RestRouter::Match m = go("GET", "/v1/fields", q);
        QCOMPARE(m.command, QString("field.list"));
        QCOMPARE(m.athlete, QString("Joe"));
        QVERIFY(!m.args.contains("athlete"));
    }

    void commandLineOnlyParametersAreRefused() {
        RestRouter::Match m = go("POST", "/v1/metrics/user", {}, "{\"file\": \"/etc/passwd\"}");
        QCOMPARE(m.httpStatus, 400);
        QVERIFY(m.error.contains("'file'"));
        QMultiMap<QString,QString> q;
        q.insert("file", "-");
        QCOMPARE(go("POST", "/v1/metrics/user", q).httpStatus, 400);
        QCOMPARE(go("POST", "/v1/commands/metric.user.add", {}, "{\"args\": {\"file\": \"/x\"}}").httpStatus, 400);
        QCOMPARE(go("POST", "/v1/metrics/user", {}, "{\"program\": \"{ value { 1; } }\"}").httpStatus, 200);

        RestRouter router(r);
        QJsonObject post = router.openApi("http://localhost:1/v1").value("paths").toObject()
                               .value("/v1/metrics/user").toObject().value("post").toObject();
        QJsonObject props = post.value("requestBody").toObject().value("content").toObject()
                                .value("application/json").toObject().value("schema").toObject()
                                .value("properties").toObject();
        QVERIFY(props.contains("program"));
        QVERIFY(!props.contains("file"));
    }

    void openApiDescribesEveryRoute() {
        RestRouter router(r);
        QJsonObject doc = router.openApi("http://localhost:1/v1");
        QCOMPARE(doc.value("openapi").toString(), QString("3.0.3"));
        QJsonObject paths = doc.value("paths").toObject();
        QVERIFY(paths.contains("/v1/athletes/{athlete}/activities/{activity}"));
        QJsonObject item = paths.value("/v1/athletes/{athlete}/activities/{activity}").toObject();
        QVERIFY(item.contains("get"));
        QVERIFY(item.contains("delete"));
        QJsonObject post = paths.value("/v1/athletes/{athlete}/imports").toObject().value("post").toObject();
        QVERIFY(post.contains("requestBody"));
        QCOMPARE(post.value("operationId").toString(), QString("import"));
    }
};

QTEST_MAIN(TestRestRouter)
#include "testRestRouter.moc"
