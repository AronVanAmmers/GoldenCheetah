TARGET = testChartRenderer
include(../common.pri)
QT += gui widgets svg
INCLUDEPATH += ../../../qwt/src
LIBS += -L$${PWD}/../../../qwt/lib -lqwt
SOURCES += testChartRenderer.cpp $$HEADLESS/ChartRenderer.cpp
