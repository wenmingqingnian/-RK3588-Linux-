if(NOT DEFINED PROJECT_ROOT)
    get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()

set(DETECTOR "${PROJECT_ROOT}/media/rknn_detector.cpp")
set(POSTPROCESS "${PROJECT_ROOT}/media/rknn_postprocess.cpp")
set(MAINWINDOW "${PROJECT_ROOT}/gui/mainwindow.cpp")
set(MONITOR "${PROJECT_ROOT}/gui/Monitor/monitor.cpp")
set(PRO_FILE "${PROJECT_ROOT}/CarTerminal.pro")
set(SETTINGS "${PROJECT_ROOT}/config/rknn_settings.conf")
foreach(path IN ITEMS
        "${DETECTOR}"
        "${POSTPROCESS}"
        "${MAINWINDOW}"
        "${MONITOR}"
        "${PRO_FILE}"
        "${SETTINGS}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "required source file missing: ${path}")
    endif()
endforeach()

file(READ "${DETECTOR}" source_raw)
file(READ "${POSTPROCESS}" postprocess_raw)
file(READ "${MAINWINDOW}" mainwindow_raw)
file(READ "${MONITOR}" monitor_raw)
file(READ "${PRO_FILE}" pro_raw)
file(READ "${SETTINGS}" settings_raw)

# Comments must never satisfy or trip executable-source contracts.
string(REGEX REPLACE "//[^\r\n]*" "" source "${source_raw}")
string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" source "${source}")
string(REGEX REPLACE "//[^\r\n]*" "" postprocess "${postprocess_raw}")
string(REGEX REPLACE "//[^\r\n]*" "" mainwindow "${mainwindow_raw}")
string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" mainwindow "${mainwindow}")
string(REGEX REPLACE "//[^\r\n]*" "" monitor "${monitor_raw}")
string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" monitor "${monitor}")
string(REGEX REPLACE "#[^\r\n]*" "" pro "${pro_raw}")
string(REGEX REPLACE "#[^\r\n]*" "" settings "${settings_raw}")

function(require_regex variable pattern description)
    if(NOT "${${variable}}" MATCHES "${pattern}")
        message(FATAL_ERROR "missing ${description}")
    endif()
endfunction()

function(reject_regex variable pattern description)
    if("${${variable}}" MATCHES "${pattern}")
        message(FATAL_ERROR "obsolete ${description} remains")
    endif()
endfunction()

require_regex(source "#[ \t]*include[ \t]*\"rknn_postprocess\\.h\"" "shared postprocess include")
require_regex(source "parseMetadata[ \t]*\\([ \t\r\n]*config\\.metadataPath" "configured metadata parse")
require_regex(source "QFile[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*\\(" "QFile model reader")
require_regex(source "open[ \t]*\\([ \t]*QIODevice::ReadOnly" "read-only model open")
require_regex(source "while[ \t]*\\([^)]*atEnd[ \t]*\\(" "incremental SHA read loop")
require_regex(source "file\\.error[ \t]*\\([ \t]*\\)[ \t]*!=[ \t]*QFileDevice::NoError" "model read-error handling")
require_regex(source "result[ \t]*\\([ \t]*\\)\\.toHex[ \t]*\\([ \t]*\\)\\.toLower[ \t]*\\(" "lowercase SHA-256 hex")
require_regex(source "validateMetadata[ \t]*\\([ \t\r\n]*s_metadata[ \t]*,[ \t\r\n]*actualModelSha" "actual model SHA metadata validation")

require_regex(source "s_inputAttrs\\[0\\]\\.fmt[ \t]*!=[ \t]*RKNN_TENSOR_NHWC" "native input NHWC rejection")
require_regex(source "s_inputAttrs\\[0\\]\\.type[ \t]*!=[ \t]*RKNN_TENSOR_FLOAT16" "native input FLOAT16 rejection")
require_regex(source "floatTensorQuantization[ \t\r\n]*\\([ \t\r\n]*s_inputAttrs\\[0\\]\\.qnt_type" "native input quantization mapping")
require_regex(source "s_inputAttrs\\[0\\]\\.n_elems[ \t]*!=[ \t]*expectedInputElems" "native input element validation")
require_regex(source "s_inputAttrs\\[0\\]\\.size[ \t]*!=[ \t]*expectedInputBytes" "native input byte validation")
require_regex(source "input\\.type[ \t]*=[ \t]*RKNN_TENSOR_UINT8" "UINT8 input submission")
require_regex(source "input\\.fmt[ \t]*=[ \t]*RKNN_TENSOR_NHWC" "NHWC input submission")
require_regex(source "input\\.pass_through[ \t]*=[^;]*s_metadata\\.callerInputPassThrough" "metadata caller pass-through submission")
reject_regex(source "input\\.fmt[ \t]*=[ \t]*s_inputFmt" "native-format input submission")
require_regex(source "outputs\\[i\\]\\.want_float[ \t]*=[ \t]*1" "FLOAT32 output request")

require_regex(source "view\\.data[ \t]*=[^;]*outputs\\[i\\]\\.buf" "OutputTensorView buffer")
require_regex(source "view\\.byteSize[ \t]*=[^;]*outputs\\[i\\]\\.size" "OutputTensorView byte size")
require_regex(source "view\\.gridWidth[ \t]*=[^;]*s_outputAttrs\\[i\\]\\.dims\\[3\\]" "OutputTensorView grid width")
require_regex(source "view\\.gridHeight[ \t]*=[^;]*s_outputAttrs\\[i\\]\\.dims\\[2\\]" "OutputTensorView grid height")
require_regex(source "view\\.outputIndex[ \t]*=[^;]*outputs\\[i\\]\\.index" "OutputTensorView output index")
require_regex(source "floatTensorQuantization[ \t\r\n]*\\([ \t\r\n]*attr\\.qnt_type" "native output quantization mapping")
string(REGEX MATCHALL "validateFloatTensorQuantization[ \t\r\n]*\\(" quantization_calls "${source}")
list(LENGTH quantization_calls quantization_call_count)
if(NOT quantization_call_count EQUAL 2)
    message(FATAL_ERROR "production must validate input and output float quantization; found ${quantization_call_count} calls")
endif()
reject_regex(source "qnt_type[ \t]*!=[ \t]*RKNN_TENSOR_QNT_NONE" "QNT_NONE-only float tensor contract")
require_regex(postprocess "FloatTensorQuantization::Affine" "identity AFFINE shared validation")
require_regex(postprocess "zeroPoint[ \t]*!=[ \t]*0" "identity AFFINE zero-point validation")
require_regex(postprocess "isfinite[ \t]*\\([ \t]*scale" "identity AFFINE finite-scale validation")
require_regex(source "attr\\.size[ \t]*!=[ \t]*expectedBytes" "native output byte validation")
require_regex(source "s_metadata\\.outputDims\\[metadataSlot\\]" "metadata-derived output dimensions")

string(REGEX MATCHALL "decodeOutputs[ \t]*\\(" decode_calls "${source}")
list(LENGTH decode_calls decode_call_count)
if(NOT decode_call_count EQUAL 1)
    message(FATAL_ERROR "production must call decodeOutputs exactly once; found ${decode_call_count}")
endif()

foreach(pattern IN ITEMS
        "static[ \t\r\n]+inline[ \t\r\n]+float[ \t\r\n]+sigmoid[ \t\r\n]*\\("
        "computeIoU[ \t\r\n]*\\("
        "nmsFilter[ \t\r\n]*\\("
        "validateYoloOutput[ \t\r\n]*\\("
        "static[ \t\r\n]+[^;{}]*decodeYoloLayer[ \t\r\n]*\\(")
    reject_regex(source "${pattern}" "local detector decoder")
endforeach()

require_regex(source "validateAndNormalizeBox[ \t]*\\([ \t\r\n]*src\\.box" "shared box validation before publication")
require_regex(postprocess "box\\.w[ \t]*<=[ \t]*0\\.0f[ \t]*\\|\\|[ \t]*box\\.h[ \t]*<=[ \t]*0\\.0f" "strictly positive box geometry")
require_regex(postprocess "numeric_limits<float>::epsilon" "scale-aware box epsilon")
require_regex(postprocess "std::clamp" "box boundary clamp")

# The Monitor singleton is not owned by MainWindow, so application teardown
# must stop it explicitly. This guarantees that an initialized RKNN context is
# released even when the Monitor page was never opened.
string(FIND "${mainwindow}" "MainWindow::~MainWindow()" destructor_pos)
if(destructor_pos EQUAL -1)
    message(FATAL_ERROR "cannot find MainWindow destructor")
endif()
string(SUBSTRING "${mainwindow}" ${destructor_pos} -1 destructor_tail)
string(FIND "${destructor_tail}" "delete ui;" destructor_end_pos)
if(destructor_end_pos EQUAL -1)
    message(FATAL_ERROR "cannot isolate MainWindow destructor")
endif()
string(SUBSTRING "${destructor_tail}" 0 ${destructor_end_pos} destructor)
require_regex(destructor
    "if[ \t\r\n]*\\([ \t\r\n]*monitor[ \t\r\n]*\\)[ \t\r\n]*\\{[ \t\r\n]*monitor->myStop[ \t\r\n]*\\([ \t\r\n]*\\)[ \t\r\n]*;"
    "null-guarded Monitor stop in MainWindow destructor")
reject_regex(destructor
    "Monitor::destroyInstance[ \t\r\n]*\\("
    "Monitor singleton destruction from MainWindow teardown")
string(FIND "${destructor}" "alarmManager.clear();" alarm_clear_pos)
string(FIND "${destructor}" "monitor->myStop();" monitor_stop_pos)
string(FIND "${destructor}" "delete buzzer;" buzzer_delete_pos)
if(alarm_clear_pos EQUAL -1 OR monitor_stop_pos EQUAL -1 OR buzzer_delete_pos EQUAL -1 OR
        NOT alarm_clear_pos LESS monitor_stop_pos OR
        NOT monitor_stop_pos LESS buzzer_delete_pos)
    message(FATAL_ERROR
        "MainWindow teardown must clear alarms, stop Monitor, then destroy buzzer")
endif()

string(FIND "${monitor}" "void Monitor::myStop()" monitor_function_pos)
string(FIND "${monitor}" "void Monitor::startCaptureButtonClicked" monitor_next_function_pos)
if(monitor_function_pos EQUAL -1 OR monitor_next_function_pos EQUAL -1 OR
        NOT monitor_function_pos LESS monitor_next_function_pos)
    message(FATAL_ERROR "cannot isolate Monitor::myStop")
endif()
math(EXPR monitor_function_length
    "${monitor_next_function_pos} - ${monitor_function_pos}")
string(SUBSTRING "${monitor}" ${monitor_function_pos} ${monitor_function_length}
    monitor_stop_function)
require_regex(monitor_stop_function
    "if[ \t\r\n]*\\([ \t\r\n]*m_rknnReady[ \t\r\n]*\\)[ \t\r\n]*\\{[^}]*rknn::deinitDetector[ \t\r\n]*\\([ \t\r\n]*\\)[ \t\r\n]*;[^}]*m_rknnReady[ \t\r\n]*=[ \t\r\n]*false[ \t\r\n]*;"
    "idempotent RKNN release and readiness clear in Monitor::myStop")
string(REGEX MATCHALL
    "rknn::deinitDetector[ \t\r\n]*\\([ \t\r\n]*\\)"
    monitor_deinit_calls "${monitor_stop_function}")
list(LENGTH monitor_deinit_calls monitor_deinit_call_count)
if(NOT monitor_deinit_call_count EQUAL 1)
    message(FATAL_ERROR
        "Monitor::myStop must call rknn::deinitDetector exactly once; found ${monitor_deinit_call_count}")
endif()

# Parse qmake continuation blocks so comments and the wrong variable cannot count.
string(REPLACE "\r\n" "\n" pro "${pro}")
string(REPLACE "\n" ";" pro_lines "${pro}")
set(scope "")
set(source_count 0)
set(header_count 0)
foreach(line IN LISTS pro_lines)
    if(line MATCHES "^[ \t]*SOURCES[ \t]*\\+=[ \t]*(.*)$")
        set(scope "SOURCES")
    elseif(line MATCHES "^[ \t]*HEADERS[ \t]*\\+=[ \t]*(.*)$")
        set(scope "HEADERS")
    endif()
    if(line MATCHES "rknn_postprocess\\.cpp")
        if(NOT scope STREQUAL "SOURCES")
            message(FATAL_ERROR "rknn_postprocess.cpp is outside SOURCES")
        endif()
        math(EXPR source_count "${source_count} + 1")
    endif()
    if(line MATCHES "rknn_postprocess\\.h")
        if(NOT scope STREQUAL "HEADERS")
            message(FATAL_ERROR "rknn_postprocess.h is outside HEADERS")
        endif()
        math(EXPR header_count "${header_count} + 1")
    endif()
    if(NOT line MATCHES "\\\\[ \t]*$")
        set(scope "")
    endif()
endforeach()
if(NOT source_count EQUAL 1 OR NOT header_count EQUAL 1)
    message(FATAL_ERROR "CarTerminal.pro must list shared cpp/header exactly once in SOURCES/HEADERS")
endif()

require_regex(settings "(^|[\r\n])[ \t]*rknn_enabled[ \t]*=[ \t]*false([ \t]*[\r\n]|$)" "rknn_enabled=false safety gate")
message(STATUS "rknn_detector production source regression passed")
