QT       += core gui widgets network multimedia

TARGET = CarTerminal
TEMPLATE = app
CONFIG += c++17

# Enable strict warnings: -Wall/-Wextra catch common mistakes,
# -Wswitch-enum makes CabinCommand::toString()'s missing-case defense effective.
QMAKE_CXXFLAGS_WARN_ON += -Wall -Wextra -Wswitch-enum
QMAKE_CXXFLAGS += -Wno-unused-parameter

# Source directories
SOURCES += main.cpp

# --- common/ ---
COMMON_DIR = $$PWD/common
SOURCES += $$COMMON_DIR/logmanager.cpp \
           $$COMMON_DIR/vehiclecommand.cpp \
           $$COMMON_DIR/alarmmanager.cpp \
           $$COMMON_DIR/alarmpolicy.cpp \
           $$COMMON_DIR/nmeaparser.cpp \
           $$COMMON_DIR/frame_buffer_pool.cpp \
           $$COMMON_DIR/thread_pool.cpp \
           $$COMMON_DIR/dmesg_tail.cpp \
           $$COMMON_DIR/attitude_estimator.cpp
HEADERS += $$COMMON_DIR/logmanager.h \
           $$COMMON_DIR/vehiclecommand.h \
           $$COMMON_DIR/alarmmanager.h \
           $$COMMON_DIR/alarmpolicy.h \
           $$COMMON_DIR/nmeaparser.h \
           $$COMMON_DIR/frame_buffer_pool.h \
           $$COMMON_DIR/thread_pool.h \
           $$COMMON_DIR/dmesg_tail.h \
           $$COMMON_DIR/attitude_estimator.h
INCLUDEPATH += $$COMMON_DIR

# --- device/ ---
DEVICE_DIR = $$PWD/device
SOURCES += $$DEVICE_DIR/vehicledeviceconfig.cpp \
           $$DEVICE_DIR/keyinputdevice.cpp \
           $$DEVICE_DIR/vehicleioinput.cpp \
           $$DEVICE_DIR/dht11device.cpp \
           $$DEVICE_DIR/dht11worker.cpp \
           $$DEVICE_DIR/dht11.cpp \
           $$DEVICE_DIR/ap3216device.cpp \
           $$DEVICE_DIR/ap3216_iiobackend.cpp \
           $$DEVICE_DIR/ap3216_useri2cbackend.cpp \
           $$DEVICE_DIR/ap3216worker.cpp \
           $$DEVICE_DIR/ap3216.cpp \
           $$DEVICE_DIR/buzzerdevice.cpp \
           $$DEVICE_DIR/mpu6050device.cpp \
           $$DEVICE_DIR/mpu6050worker.cpp \
           $$DEVICE_DIR/mpu6050_iiobuffersource.cpp \
           $$DEVICE_DIR/mpu6050.cpp \
           $$DEVICE_DIR/gpsdevice.cpp \
           $$DEVICE_DIR/gpssimulator.cpp \
           $$DEVICE_DIR/gpsworker.cpp \
           $$DEVICE_DIR/i2cbusses.c \
           $$DEVICE_DIR/smbus.c
HEADERS += $$DEVICE_DIR/vehicledeviceconfig.h \
           $$DEVICE_DIR/keyinputdevice.h \
           $$DEVICE_DIR/vehicleioinput.h \
           $$DEVICE_DIR/dht11device.h \
           $$DEVICE_DIR/dht11worker.h \
           $$DEVICE_DIR/dht11.h \
           $$DEVICE_DIR/ap3216device.h \
           $$DEVICE_DIR/ap3216_iiobackend.h \
           $$DEVICE_DIR/ap3216_useri2cbackend.h \
           $$DEVICE_DIR/ap3216worker.h \
           $$DEVICE_DIR/ap3216.h \
           $$DEVICE_DIR/buzzerdevice.h \
           $$DEVICE_DIR/mpu6050device.h \
           $$DEVICE_DIR/mpu6050worker.h \
           $$DEVICE_DIR/iimusamplesource.h \
           $$DEVICE_DIR/mpu6050_iiobuffersource.h \
           $$DEVICE_DIR/mpu6050.h \
           $$DEVICE_DIR/gpsdevice.h \
           $$DEVICE_DIR/gpssimulator.h \
           $$DEVICE_DIR/gpsworker.h \
           $$DEVICE_DIR/i2cbusses.h \
           $$DEVICE_DIR/smbus.h
INCLUDEPATH += $$DEVICE_DIR

# --- media/ ---
MEDIA_DIR = $$PWD/media
SOURCES += $$MEDIA_DIR/cameraconfig.cpp \
           $$MEDIA_DIR/camera_router.cpp \
           $$MEDIA_DIR/capture_thread.cpp \
           $$MEDIA_DIR/v4l2_frame_telemetry.cpp \
           $$MEDIA_DIR/v4l2_capture_utils.cpp \
           $$MEDIA_DIR/rga_converter.cpp \
           $$MEDIA_DIR/mpp_decoder.cpp \
           $$MEDIA_DIR/mpp_encoder.cpp \
           $$MEDIA_DIR/rknn_detector.cpp \
           $$MEDIA_DIR/rknn_postprocess.cpp \
           $$MEDIA_DIR/rknn_runtime.cpp \
           $$MEDIA_DIR/rtsp_server.cpp \
           $$MEDIA_DIR/mp4_recorder.cpp \
           $$MEDIA_DIR/test_pattern_source.cpp \
           $$MEDIA_DIR/file_frame_source.cpp \
           $$MEDIA_DIR/frame_broker.cpp \
           $$MEDIA_DIR/media_pipeline.cpp
HEADERS += $$MEDIA_DIR/cameraconfig.h \
           $$MEDIA_DIR/camera_router.h \
           $$MEDIA_DIR/capture_thread.h \
           $$MEDIA_DIR/v4l2_frame_telemetry.h \
           $$MEDIA_DIR/v4l2_capture_utils.h \
           $$MEDIA_DIR/rga_converter.h \
           $$MEDIA_DIR/mpp_decoder.h \
           $$MEDIA_DIR/mpp_abi_constants.h \
           $$MEDIA_DIR/mpp_encoder.h \
           $$MEDIA_DIR/mpp_frame_layout.h \
           $$MEDIA_DIR/rknn_detector.h \
           $$MEDIA_DIR/rknn_postprocess.h \
           $$MEDIA_DIR/rknn_runtime.h \
           $$MEDIA_DIR/rtsp_server.h \
           $$MEDIA_DIR/mp4_recorder.h \
           $$MEDIA_DIR/frame_packet.h \
           $$MEDIA_DIR/frame_source.h \
           $$MEDIA_DIR/test_pattern_source.h \
           $$MEDIA_DIR/file_frame_source.h \
           $$MEDIA_DIR/frame_broker.h \
           $$MEDIA_DIR/media_pipeline.h
INCLUDEPATH += $$MEDIA_DIR

# RKNN: use board-installed OFFICIAL rknn_api.h (jilu.txt §六.1, §四.1).
# Hand-written ABI is forbidden. On hosts without rknn_api.h, rknn_detector.cpp
# falls back to a disabled stub via #if __has_include(<rknn_api.h>).
# RKNN: prefer the official header from the active Qt/Buildroot sysroot.
# Native-board include paths remain available as fallbacks.
RKNN_SYSROOT = $$[QT_SYSROOT]
RKNN_INCLUDE_DIR =

!isEmpty(RKNN_SYSROOT) {
    exists($$RKNN_SYSROOT/usr/include/rknn/rknn_api.h) {
        RKNN_INCLUDE_DIR = $$RKNN_SYSROOT/usr/include/rknn
    }
}

isEmpty(RKNN_SYSROOT):isEmpty(RKNN_INCLUDE_DIR):linux:exists(/usr/include/rknn/rknn_api.h) {
    RKNN_INCLUDE_DIR = /usr/include/rknn
}

isEmpty(RKNN_SYSROOT):isEmpty(RKNN_INCLUDE_DIR):linux:exists(/usr/local/include/rknn_api.h) {
    RKNN_INCLUDE_DIR = /usr/local/include
}

!isEmpty(RKNN_INCLUDE_DIR) {
    message("RKNN: using official rknn_api.h from $$RKNN_INCLUDE_DIR")
    INCLUDEPATH += $$RKNN_INCLUDE_DIR
} else {
    message("RKNN: official rknn_api.h not found — building disabled stub")
}

# RGA: link against board-installed librga2 (im2d API) when available.
# Uses /usr/include/rga/ headers and -lrga (alias for librga.so -> librga2.so).
# RGA: prefer headers and libraries from the active Qt/Buildroot sysroot.
RGA_SYSROOT = $$[QT_SYSROOT]
RGA_INCLUDE_DIR =

!isEmpty(RGA_SYSROOT) {
    exists($$RGA_SYSROOT/usr/include/rga/im2d.h) {
        RGA_INCLUDE_DIR = $$RGA_SYSROOT/usr/include/rga
    }
}

isEmpty(RGA_SYSROOT):isEmpty(RGA_INCLUDE_DIR):linux:exists(/usr/include/rga/im2d.h) {
    RGA_INCLUDE_DIR = /usr/include/rga
}

!isEmpty(RGA_INCLUDE_DIR) {
    message("RGA: using im2d API from $$RGA_INCLUDE_DIR")
    INCLUDEPATH += $$RGA_INCLUDE_DIR
    LIBS += -lrga
} else {
    message("RGA: im2d.h not found — building CPU-only fallback")
}

# dlopen / MPP dynamic loading requires libdl on Linux
linux: LIBS += -ldl

# ZMQ library for voice inter-process communication
linux: LIBS += -lzmq

# FFmpeg libraries:
#   -lavcodec        codec abstractions (used by software fallback encoder)
#   -lswscale        RGB32 → YUV420P color conversion
#   -lavutil         common utilities (mem, rational, etc.)
#   -lavformat       MP4 muxer (avformat_alloc_output_context2 / av_write_frame)
#                    — used by Mp4Recorder
linux: LIBS += -lavcodec -lswscale -lavutil -lavformat

# ALSA library for audio capture
linux: LIBS += -lasound

# --- voice/ ---
VOICE_DIR = $$PWD/voice
SOURCES += $$VOICE_DIR/VoiceManager.cpp \
           $$VOICE_DIR/voice_config.cpp \
           $$VOICE_DIR/asr/AsrEngine.cpp \
           $$VOICE_DIR/kws/KwsEngine.cpp \
           $$VOICE_DIR/zmq/src/ZmqInterface.cpp \
           $$VOICE_DIR/zmq/src/ZmqServer.cpp \
           $$VOICE_DIR/zmq/src/ZmqClient.cpp
HEADERS += $$VOICE_DIR/VoiceManager.h \
           $$VOICE_DIR/voice_config.h \
           $$VOICE_DIR/asr/AsrEngine.h \
           $$VOICE_DIR/kws/KwsEngine.h \
           $$VOICE_DIR/zmq/include/ZmqInterface.h \
           $$VOICE_DIR/zmq/include/ZmqServer.h \
           $$VOICE_DIR/zmq/include/ZmqClient.h
INCLUDEPATH += $$VOICE_DIR
INCLUDEPATH += $$VOICE_DIR/asr
INCLUDEPATH += $$VOICE_DIR/kws
INCLUDEPATH += $$VOICE_DIR/zmq/include

# --- network/ ---
NETWORK_DIR = $$PWD/network
SOURCES += $$NETWORK_DIR/staticmapconfig.cpp \
           $$NETWORK_DIR/staticmapresponse.cpp
HEADERS += $$NETWORK_DIR/staticmapconfig.h \
           $$NETWORK_DIR/staticmapresponse.h
INCLUDEPATH += $$NETWORK_DIR

# --- gui/ ---
GUI_DIR = $$PWD/gui
SOURCES += $$GUI_DIR/mainwindow.cpp \
           $$GUI_DIR/settingwindow.cpp \
           $$GUI_DIR/settinggifscanner.cpp \
           $$GUI_DIR/clock.cpp \
           $$GUI_DIR/speechrecognition.cpp \
           $$GUI_DIR/Map/baidumap.cpp \
           $$GUI_DIR/Music/musicplayer.cpp \
           $$GUI_DIR/Music/searchmusic.cpp \
           $$GUI_DIR/Weather/weather.cpp \
           $$GUI_DIR/Weather/weatherconfig.cpp \
           $$GUI_DIR/Weather/weatherformat.cpp \
           $$GUI_DIR/Monitor/monitor.cpp
HEADERS += $$GUI_DIR/mainwindow.h \
           $$GUI_DIR/settingwindow.h \
           $$GUI_DIR/settinggifscanner.h \
           $$GUI_DIR/clock.h \
           $$GUI_DIR/speechrecognition.h \
           $$GUI_DIR/Map/baidumap.h \
           $$GUI_DIR/Music/musicplayer.h \
           $$GUI_DIR/Music/searchmusic.h \
           $$GUI_DIR/Weather/weather.h \
           $$GUI_DIR/Weather/weatherconfig.h \
           $$GUI_DIR/Weather/weatherformat.h \
           $$GUI_DIR/Monitor/monitor.h
INCLUDEPATH += $$GUI_DIR
INCLUDEPATH += $$GUI_DIR/Map
INCLUDEPATH += $$GUI_DIR/Music
INCLUDEPATH += $$GUI_DIR/Weather
INCLUDEPATH += $$GUI_DIR/Monitor

FORMS += $$GUI_DIR/mainwindow.ui \
         $$GUI_DIR/settingwindow.ui \
         $$GUI_DIR/clock.ui \
         $$GUI_DIR/Map/baidumap.ui \
         $$GUI_DIR/Music/musicplayer.ui \
         $$GUI_DIR/Music/searchmusic.ui \
         $$GUI_DIR/Weather/weather.ui \
         $$GUI_DIR/Monitor/monitor.ui

RESOURCES += $$PWD/resources/img.qrc
