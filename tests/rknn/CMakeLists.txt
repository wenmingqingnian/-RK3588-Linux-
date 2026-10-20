# tests/rknn/CMakeLists.txt
# Build for the RKNN minimal inference probe (jilu.txt §四, §七) and the
# Stage B static-image test tool (jilu.txt §4-§9).
#
# Usage:
#   cmake -S tests/rknn -B build_rknn \
#         -DCMAKE_BUILD_TYPE=Release \
#         -DRKNN_INCLUDE_DIR=/usr/local/include
#   cmake --build build_rknn --parallel 1
#   ctest --test-dir build_rknn --output-on-failure
#
# This CMake project is deliberately self-contained: it does NOT include any
# OpenCV, RGA, MPP, RKLLM, ALSA, or camera sources.
#
# Targets:
#   test_rknn_image_common — pure-logic TDD tests (no NPU/Qt/RKNN deps)
#   test_rknn_postprocess_adapter — multi-output adapter contract tests
#   rknn_probe             — minimal inference probe (rknn_api.h + libdl)
#   rknn_image_test        — Stage B CLI tool (rknn_api.h + Qt5 Core/Gui)

cmake_minimum_required(VERSION 3.10)
project(rknn_probe LANGUAGES CXX)

# ---- C++17 + strict warnings (jilu.txt §七) ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---- Locate rknn_api.h (optional for test_rknn_image_common) ----
# Default to /usr/local/include (jilu.txt §一.2). Override with -DRKNN_INCLUDE_DIR.
if(NOT DEFINED RKNN_INCLUDE_DIR)
    set(RKNN_INCLUDE_DIR "/usr/local/include" CACHE STRING "Path to directory containing rknn_api.h")
endif()

if(EXISTS "${RKNN_INCLUDE_DIR}/rknn_api.h")
    set(RKNN_HEADER_FOUND TRUE)
    message(STATUS "RKNN_INCLUDE_DIR = ${RKNN_INCLUDE_DIR}")
    message(STATUS "RKNN header      = ${RKNN_INCLUDE_DIR}/rknn_api.h")
else()
    set(RKNN_HEADER_FOUND FALSE)
    message(WARNING
        "rknn_api.h not found at '${RKNN_INCLUDE_DIR}/rknn_api.h'.\n"
        "Targets requiring RKNN (rknn_probe, rknn_image_test) will be skipped.\n"
        "test_rknn_image_common can still be built (pure-logic TDD).\n"
        "Hand-writing the RKNN ABI is forbidden (jilu.txt §P0-1, §四.1).")
endif()

# ---- Common sources ----
# rknn_runtime is Qt-free (jilu.txt §四): it lives under ../../media/ but only
# pulls in rknn_api.h + standard C/C++ headers.
set(RKNN_RUNTIME_SOURCES
    ../../media/rknn_runtime.cpp
)

# ============================================================
# test_rknn_image_common — pure-logic TDD tests (jilu.txt §5)
# No NPU, no Qt, no RKNN header, no libdl. Host-compilable for TDD.
# ============================================================
add_executable(test_rknn_image_common
    test_rknn_image_common.cpp
    ../../media/rknn_postprocess.cpp
)
target_include_directories(test_rknn_image_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../media
)
# No target_link_libraries — only C++17 standard library.

# ============================================================
# test_rknn_postprocess_adapter — production multi-output adapter TDD
# Qt-free host contract test for the shared production implementation.
# ============================================================
add_executable(test_rknn_postprocess_adapter
    test_rknn_postprocess_adapter.cpp
    ../../media/rknn_postprocess.cpp
)
target_include_directories(test_rknn_postprocess_adapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../media
)
target_compile_definitions(test_rknn_postprocess_adapter PRIVATE
    RKNN_TEST_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../.."
)

# ============================================================
# rknn_probe — minimal inference probe (Stage A, jilu.txt §四)
# ============================================================
if(RKNN_HEADER_FOUND)
    add_executable(rknn_probe
        rknn_probe.cpp
        ${RKNN_RUNTIME_SOURCES}
    )
    target_include_directories(rknn_probe PRIVATE
        ${RKNN_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../media
    )
    # Only -ldl is needed (jilu.txt §七). No Qt, no RKLLM, no ALSA, no camera.
    target_link_libraries(rknn_probe PRIVATE ${CMAKE_DL_LIBS})
endif()

# ============================================================
# rknn_image_test — Stage B CLI tool (jilu.txt §4-§9)
# Allows Qt5 Core/Gui (QImage, QPainter, QCryptographicHash).
# Forbids QWidget, OpenCV, camera, RGA/MPP/RKLLM/ALSA.
# Read-only reuses media/rknn_runtime.cpp.
# ============================================================
if(RKNN_HEADER_FOUND)
    find_package(Qt5 COMPONENTS Core Gui QUIET)
    if(Qt5_FOUND)
        add_executable(rknn_image_test
            rknn_image_test.cpp
            ../../media/rknn_postprocess.cpp
            ${RKNN_RUNTIME_SOURCES}
        )
        target_include_directories(rknn_image_test PRIVATE
            ${RKNN_INCLUDE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../media
            ${CMAKE_CURRENT_SOURCE_DIR}
        )
        target_link_libraries(rknn_image_test PRIVATE
            ${CMAKE_DL_LIBS}
            Qt5::Core
            Qt5::Gui
        )
        # Disable undefined symbols at link time (catches missing deps early).
        set_target_properties(rknn_image_test PROPERTIES
            LINK_FLAGS "-Wl,--no-undefined")
    else()
        message(WARNING
            "Qt5 Core/Gui not found; rknn_image_test skipped.\n"
            "Install qtbase5-dev on the board to enable Stage B CLI.")
    endif()
endif()

# ============================================================
# CTest registration (jilu.txt §5: TDD)
# ============================================================
enable_testing()
add_test(NAME test_rknn_detector_source
    COMMAND ${CMAKE_COMMAND}
        -DPROJECT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/../..
        -P ${CMAKE_CURRENT_SOURCE_DIR}/test_rknn_detector_source.cmake)
add_test(NAME test_rknn_image_common COMMAND test_rknn_image_common)
add_test(NAME test_rknn_postprocess_adapter
         COMMAND test_rknn_postprocess_adapter)
add_test(NAME test_rknn_image_stageb_source
    COMMAND ${CMAKE_COMMAND}
        -DSTAGEB_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/rknn_image_test.cpp
        -P ${CMAKE_CURRENT_SOURCE_DIR}/test_rknn_image_stageb_source.cmake)

# ============================================================
# Install (optional, useful for board deployment)
# ============================================================
include(GNUInstallDirs)
install(TARGETS test_rknn_image_common
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(RKNN_HEADER_FOUND AND TARGET rknn_probe)
    install(TARGETS rknn_probe
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(RKNN_HEADER_FOUND AND TARGET rknn_image_test)
    install(TARGETS rknn_image_test
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
