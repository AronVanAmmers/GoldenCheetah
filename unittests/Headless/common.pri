# shared by the headless unit tests: they compile the (dependency free)
# headless sources they test directly, like splineCrash does
QT += testlib core
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app

include($$PWD/../unittests.pri)

HEADLESS = $$PWD/../../src/Headless
INCLUDEPATH += $$HEADLESS $$PWD
