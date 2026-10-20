cmake_minimum_required(VERSION 3.16)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

file(READ "${PROJECT_ROOT}/main.cpp" MAIN_SOURCE)
file(READ "${PROJECT_ROOT}/gui/mainwindow.cpp" MAIN_WINDOW_SOURCE)
file(READ "${PROJECT_ROOT}/gui/Monitor/monitor.h" MONITOR_HEADER)
file(READ "${PROJECT_ROOT}/gui/Monitor/monitor.cpp" MONITOR_SOURCE)
file(READ "${PROJECT_ROOT}/gui/Map/baidumap.cpp" MAP_SOURCE)
file(READ "${PROJECT_ROOT}/gui/Music/musicplayer.cpp" MUSIC_SOURCE)
file(READ "${PROJECT_ROOT}/device/ap3216.cpp" AP3216_SOURCE)
file(READ "${PROJECT_ROOT}/gui/settingwindow.cpp" SETTING_SOURCE)
file(READ "${PROJECT_ROOT}/voice/asr/AsrEngine.h" ASR_HEADER)
file(READ "${PROJECT_ROOT}/voice/asr/AsrEngine.cpp" ASR_SOURCE)
file(READ "${PROJECT_ROOT}/media/capture_thread.cpp" CAPTURE_SOURCE)

function(require_contains CONTENT NEEDLE MESSAGE_TEXT)
    string(FIND "${CONTENT}" "${NEEDLE}" POSITION)
    if(POSITION EQUAL -1)
        message(FATAL_ERROR "${MESSAGE_TEXT}")
    endif()
endfunction()

function(require_not_contains CONTENT NEEDLE MESSAGE_TEXT)
    string(FIND "${CONTENT}" "${NEEDLE}" POSITION)
    if(NOT POSITION EQUAL -1)
        message(FATAL_ERROR "${MESSAGE_TEXT}")
    endif()
endfunction()

require_contains("${MAIN_SOURCE}" "volatile std::sig_atomic_t g_signalReceived"
                 "signal handoff must use volatile sig_atomic_t")
require_contains("${MONITOR_HEADER}" "static void destroyInstance();"
                 "Monitor must expose explicit singleton destruction")
require_contains("${MONITOR_SOURCE}" "void Monitor::destroyInstance()"
                 "Monitor singleton destruction must be implemented")
require_contains("${MAIN_WINDOW_SOURCE}" "Monitor::destroyInstance();"
                 "MainWindow shutdown must destroy the Monitor singleton")

require_not_contains("${MAP_SOURCE}" "map request: %s"
                     "map API-key-bearing URL must not be logged")
require_not_contains("${MUSIC_SOURCE}" "setPeerVerifyMode(QSslSocket::VerifyNone)"
                     "TLS peer verification must not be disabled")
require_not_contains("${AP3216_SOURCE}" "qDebug()"
                     "AP3216 must use the unified logger")
require_not_contains("${AP3216_SOURCE}" "qWarning()"
                     "AP3216 must use the unified logger")
require_not_contains("${SETTING_SOURCE}" "qWarning()"
                     "settings UI must use the unified logger")
require_contains("${ASR_HEADER}"
                 "using FnGetResult = const sherpa_abi_1_13_3::OnlineRecognizerResult*"
                 "ASR result function pointer must be typed")
require_contains("${ASR_SOURCE}"
                 "if (!loadRuntimeVersion() || !m_resultAbiCompatible)"
                 "ASR must reject an unverified runtime before passing ABI structs")
require_not_contains("${ASR_SOURCE}" "const void *rawResult"
                     "ASR result must not be handled through an untyped pointer")
require_contains("${ASR_SOURCE}" "const bool hadResources ="
                 "ASR deinit must release partially initialized resources")
require_contains("${CAPTURE_SOURCE}" "lock.unlock();\n    emit stateChanged(newState);"
                 "capture state signals must be emitted after releasing the state mutex")

message(STATUS "audit source contracts passed")
