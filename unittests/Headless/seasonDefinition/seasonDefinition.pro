TARGET = testSeasonDefinition
include(../common.pri)
CORE = $$PWD/../../../src/Core
INCLUDEPATH += $$CORE
SOURCES += testSeasonDefinition.cpp $$HEADLESS/SeasonDefinition.cpp $$CORE/Season.cpp
