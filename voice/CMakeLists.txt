cmake_minimum_required(VERSION 3.10)
project(voice_services CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# RPATH: allow executables to find libzmq_component.so in ../lib at runtime
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)

enable_testing()

# --- ZMQ communication library ---

set(ZMQ_SOURCES
    zmq/src/ZmqInterface.cpp
    zmq/src/ZmqServer.cpp
    zmq/src/ZmqClient.cpp
)

add_library(zmq_component SHARED ${ZMQ_SOURCES})
target_include_directories(zmq_component PUBLIC zmq/include)
target_link_libraries(zmq_component PRIVATE zmq)

# --- LLM service executable ---

set(LLM_SOURCES
    llm/src/llm_demo.cpp
)

add_executable(llm_service ${LLM_SOURCES})
target_include_directories(llm_service PRIVATE
    llm/include
    zmq/include
)
target_link_libraries(llm_service
    zmq_component
    zmq
    rkllmrt
    pthread
)
target_compile_options(llm_service PRIVATE -O2 -Wall -Wextra)

# --- SummerTTS engine static library ---
# Shared by tts_service and tts_wav_test. Contains the synthesis core
# (TTSModel + SummerTTS internals + Text Normalization). Excludes service
# infrastructure (main.cpp, AudioPlayer.cpp, MessageQueue.cpp, TextProcessor.cpp)
# so that tts_wav_test can reuse the engine without linking ALSA or ZMQ.

# SummerTTS engine internal source directories.
# NOTE: file(GLOB) is used because SummerTTS has many source dirs.
# If new files are added, cmake must be re-run.
file(GLOB SUMMER_TTS_MODELS   tts/src/models/*.cpp)
file(GLOB SUMMER_TTS_MODULES  tts/src/modules/*.cpp)
file(GLOB SUMMER_TTS_NN_OP    tts/src/nn_op/*.cpp)
file(GLOB SUMMER_TTS_HZ2PY    tts/src/hz2py/*.cpp)
file(GLOB SUMMER_TTS_ENGIPA   tts/src/engipa/*.cpp)
file(GLOB SUMMER_TTS_PLATFORM tts/src/platform/*.cpp)
file(GLOB SUMMER_TTS_UTILS    tts/src/utils/*.cpp)

# TN (Text Normalization) source files
set(TN_SOURCES
    tts/src/tn/glog/src/demangle.cc
    tts/src/tn/glog/src/logging.cc
    tts/src/tn/glog/src/raw_logging.cc
    tts/src/tn/glog/src/symbolize.cc
    tts/src/tn/glog/src/utilities.cc
    tts/src/tn/glog/src/vlog_is_on.cc
    tts/src/tn/glog/src/signalhandler.cc
    tts/src/tn/gflags/src/gflags.cc
    tts/src/tn/gflags/src/gflags_reporting.cc
    tts/src/tn/gflags/src/gflags_completions.cc
    tts/src/tn/openfst/src/lib/compat.cc
    tts/src/tn/openfst/src/lib/flags.cc
    tts/src/tn/openfst/src/lib/fst.cc
    tts/src/tn/openfst/src/lib/fst-types.cc
    tts/src/tn/openfst/src/lib/mapped-file.cc
    tts/src/tn/openfst/src/lib/properties.cc
    tts/src/tn/openfst/src/lib/symbol-table.cc
    tts/src/tn/openfst/src/lib/symbol-table-ops.cc
    tts/src/tn/openfst/src/lib/util.cc
    tts/src/tn/openfst/src/lib/weight.cc
    tts/src/tn/processor.cc
    tts/src/tn/token_parser.cc
    tts/src/tn/utf8_string.cc
)

set(TTS_ENGINE_SOURCES
    tts/src/TTSModel.cpp
    tts/src/Utils.cpp
    ${SUMMER_TTS_MODELS}
    ${SUMMER_TTS_MODULES}
    ${SUMMER_TTS_NN_OP}
    ${SUMMER_TTS_HZ2PY}
    ${SUMMER_TTS_ENGIPA}
    ${SUMMER_TTS_PLATFORM}
    ${SUMMER_TTS_UTILS}
    ${TN_SOURCES}
)

add_library(summer_tts_engine STATIC ${TTS_ENGINE_SOURCES})
target_include_directories(summer_tts_engine PUBLIC
    tts/include
    tts/src/header
    tts/src/tn/header
    tts/src/hz2py
    tts/src/engipa
    tts/eigen-3.4.0
)
target_compile_options(summer_tts_engine PRIVATE -O2 -Wall -Wextra)

# --- TTS service executable ---

add_executable(tts_service
    tts/src/main.cpp
    tts/src/AudioPlayer.cpp
    tts/src/MessageQueue.cpp
    tts/src/TextProcessor.cpp
)
target_include_directories(tts_service PRIVATE
    zmq/include
)
target_link_libraries(tts_service
    summer_tts_engine
    zmq_component
    zmq
    asound
    pthread
)
target_compile_options(tts_service PRIVATE -O2 -Wall -Wextra)

# --- PCM16 WAV I/O module (shared by tools and tests) ---

add_library(wav_io STATIC audio/wav_io.cpp)
target_include_directories(wav_io PUBLIC audio)

# --- tts_wav_test: TTS -> WAV command-line tool ---
# No ALSA, no ZMQ, no AudioPlayer. Reuses SummerTTS engine directly.

add_executable(tts_wav_test
    tools/tts_wav_test.cpp
)
target_link_libraries(tts_wav_test
    summer_tts_engine
    wav_io
    pthread
)
target_compile_options(tts_wav_test PRIVATE -Wall -Wextra)

# --- asr_wav_test: WAV -> ASR -> CabinCommand command-line tool ---
# No ALSA, no Qt GUI. Reuses AsrEngine (dlopen sherpa-onnx), wav_io,
# vehiclecommand and logmanager from ../common.

add_executable(asr_wav_test
    tools/asr_wav_test.cpp
    asr/AsrEngine.cpp
    ../common/vehiclecommand.cpp
    ../common/logmanager.cpp
)
target_include_directories(asr_wav_test PRIVATE
    asr
    ../common
)
target_link_libraries(asr_wav_test
    wav_io
    dl
    pthread
)
target_compile_options(asr_wav_test PRIVATE -Wall -Wextra)

# --- WAV I/O unit tests ---

add_executable(test_wav_io
    tests/test_wav_io.cpp
)
target_link_libraries(test_wav_io
    wav_io
)
target_compile_options(test_wav_io PRIVATE -Wall -Wextra)
add_test(NAME wav_io_tests COMMAND test_wav_io)

# --- sherpa-onnx 1.13.3 ABI compatibility unit tests ---
# 纯头文件测试，不依赖 sherpa 动态库、不依赖板端模型，可在任意支持 C++17 的机器运行。

add_executable(test_asr_abi
    tests/test_asr_abi.cpp
)
target_include_directories(test_asr_abi PRIVATE
    asr
)
target_compile_options(test_asr_abi PRIVATE -Wall -Wextra)
add_test(NAME asr_abi_tests COMMAND test_asr_abi)

# --- vehiclecommand 单元测试 ---
# 验证 parseCabinCommand 指令匹配规则（含孤立 "播放" 拒绝执行、
# "播放音乐" 完整匹配和 "打开"+"摄象" 组合匹配）。
# 纯 C++17，不依赖外部库。

add_executable(test_vehiclecommand
    tests/test_vehiclecommand.cpp
    ../common/vehiclecommand.cpp
)
target_include_directories(test_vehiclecommand PRIVATE
    ../common
)
target_compile_options(test_vehiclecommand PRIVATE -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8)
add_test(NAME vehiclecommand_tests COMMAND test_vehiclecommand)

# --- AsrEngine 配置解析单元测试（热词解码配置）---
# 验证 AsrEngine::loadConfig() 对 asr_decoding_method / asr_hotwords_file /
# asr_hotwords_score 的解析、默认值与非法值回退。
# 复用 AsrEngine.cpp 与 ../common/logmanager.cpp，不链接 Qt/ALSA/ZMQ/RKNN/NPU。

add_executable(test_asr_config
    tests/test_asr_config.cpp
    asr/AsrEngine.cpp
    ../common/logmanager.cpp
)
target_include_directories(test_asr_config PRIVATE
    asr
    ../common
)
target_link_libraries(test_asr_config
    dl
    pthread
)
target_compile_options(test_asr_config PRIVATE -Wall -Wextra)
add_test(NAME asr_config_tests COMMAND test_asr_config)

# --- Install targets (production only; test tools stay in build dir) ---

install(TARGETS zmq_component llm_service tts_service
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
)
