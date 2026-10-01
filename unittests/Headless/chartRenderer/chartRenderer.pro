TARGET = testChartRenderer
include(../common.pri)
QT += gui widgets svg
INCLUDEPATH += ../../../qwt/src
# as src.pro: Windows builds a debug qwt of its own
win32:CONFIG(debug, debug|release) {
    LIBS += -L$${PWD}/../../../qwt/lib -lqwtd
} else {
    LIBS += -L$${PWD}/../../../qwt/lib -lqwt
}
SOURCES += testChartRenderer.cpp $$HEADLESS/ChartRenderer.cpp
