TARGET = testCliParser
include(../common.pri)
SOURCES += testCliParser.cpp $$HEADLESS/CliParser.cpp $$HEADLESS/CommandRegistry.cpp
