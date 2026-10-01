# shared by the headless unit tests: they compile the (dependency free)
# headless sources they test directly, like splineCrash does
QT += testlib core
CONFIG += console
CONFIG -= app_bundle
TEMPLATE = app

include($$PWD/../unittests.pri)

# unittests.pri asks for c++11, the sources need c++17 (as Qt 6 does)
CONFIG -= c++11
CONFIG += c++17

HEADLESS = $$PWD/../../src/Headless
INCLUDEPATH += $$HEADLESS $$PWD
