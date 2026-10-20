QT += core

CONFIG += console c++17
CONFIG -= app_bundle

TEMPLATE = app
TARGET = test_camera_router_lifecycle

INCLUDEPATH += ../media

SOURCES += \
    test_camera_router_lifecycle.cpp \
    ../media/camera_router.cpp

HEADERS += ../media/camera_router.h
