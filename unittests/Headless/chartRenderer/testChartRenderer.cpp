#include "ChartRenderer.h"

#include <QTest>
#include <QApplication>
#include <QImage>

using namespace Headless;

class TestChartRenderer : public QObject
{
    Q_OBJECT

    ChartSpec sample(ChartPanel::XAxis axis = ChartPanel::Duration) {
        ChartSpec spec;
        spec.title = "Test";
        spec.size = QSize(640, 360);
        ChartPanel panel;
        panel.xAxis = axis;
        panel.yLabel = "Power (W)";
        ChartSeries s;
        s.name = "Power";
        s.color = QColor(255, 170, 0);
        for (int i = 1; i <= 600; i++) { s.x << i; s.y << 200 + 50 * std::sin(i / 30.0); }
        panel.series << s;
        spec.panels << panel;
        return spec;
    }

private slots:

    void png() {
        QString error;
        QByteArray bytes = ChartRenderer::render(sample(), "png", error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(bytes.startsWith("\x89PNG"));
        QImage img = QImage::fromData(bytes, "PNG");
        QCOMPARE(img.size(), QSize(640, 360));

        // something was drawn in the series colour
        bool found = false;
        for (int y = 0; y < img.height() && !found; y += 2)
            for (int x = 0; x < img.width() && !found; x += 2) {
                QColor c = img.pixelColor(x, y);
                if (c.red() > 200 && c.green() > 120 && c.green() < 220 && c.blue() < 80) found = true;
            }
        QVERIFY(found);
    }

    void svgAndPdf() {
        QString error;
        QByteArray svg = ChartRenderer::render(sample(), "svg", error);
        QVERIFY(svg.contains("<svg"));
        QByteArray pdf = ChartRenderer::render(sample(), "pdf", error);
        QVERIFY(pdf.startsWith("%PDF"));
    }

    void everyAxisKindAndStyle() {
        QString error;
        for (ChartPanel::XAxis axis : { ChartPanel::Plain, ChartPanel::Duration, ChartPanel::LogDuration, ChartPanel::Date }) {
            QVERIFY(!ChartRenderer::render(sample(axis), "png", error).isEmpty());
        }
        ChartSpec bars = sample(ChartPanel::Categories);
        bars.panels[0].categories = QStringList{ "Z1", "Z2", "Z3" };
        bars.panels[0].series[0].style = ChartSeries::Bars;
        bars.panels[0].series[0].x = { 0, 1, 2 };
        bars.panels[0].series[0].y = { 3, 5, 1 };
        QVERIFY(!ChartRenderer::render(bars, "png", error).isEmpty());

        ChartSpec stacked = sample();
        stacked.panels << stacked.panels[0];
        stacked.panels[1].series[0].rightAxis = true;
        stacked.panels[1].series[0].style = ChartSeries::Area;
        stacked.dark = true;
        QVERIFY(!ChartRenderer::render(stacked, "png", error).isEmpty());
    }

    void rejectsBadRequests() {
        QString error;
        QVERIFY(ChartRenderer::render(sample(), "gif", error).isEmpty());
        QVERIFY(error.contains("png, svg or pdf"));
        ChartSpec empty;
        QVERIFY(ChartRenderer::render(empty, "png", error).isEmpty());
        ChartSpec tiny = sample();
        tiny.size = QSize(10, 10);
        QVERIFY(ChartRenderer::render(tiny, "png", error).isEmpty());
    }

    void mimeTypes() {
        QCOMPARE(ChartRenderer::mimeType("png"), QString("image/png"));
        QCOMPARE(ChartRenderer::mimeType("svg"), QString("image/svg+xml"));
        QCOMPARE(ChartRenderer::mimeType("pdf"), QString("application/pdf"));
    }
};

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestChartRenderer test;
    return QTest::qExec(&test, argc, argv);
}

#include "testChartRenderer.moc"
