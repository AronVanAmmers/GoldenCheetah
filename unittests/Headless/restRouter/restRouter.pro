TARGET = testRestRouter
include(../common.pri)
SOURCES += testRestRouter.cpp $$HEADLESS/RestRouter.cpp $$HEADLESS/CommandRegistry.cpp
