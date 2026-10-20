#!/bin/bash
# verify_tts_asr_wav.sh
#
# TTS -> WAV -> ASR -> CabinCommand 批量验证脚本（无硬件闭环）。
#
# 用法（在项目根目录 <PROJECT_DIR> 下执行）：
#   ./voice/scripts/verify_tts_asr_wav.sh
#   ./voice/scripts/verify_tts_asr_wav.sh --profile smoke
#   ./voice/scripts/verify_tts_asr_wav.sh --profile accuracy
#   ./voice/scripts/verify_tts_asr_wav.sh --keep-success
#   ./voice/scripts/verify_tts_asr_wav.sh --build-dir build_voice_tools \
#       --model ./config/single_speaker_fast.bin \
#       --config ./config/asr_settings_offline.conf
#
# 退出码：
#   0 全部通过
#   1 至少一个测试失败
#   2 脚本参数错误或依赖缺失

set -euo pipefail

# --- 默认路径（相对于项目根目录） ---
BUILD_DIR="./build_voice_tools"
MODEL_PATH="./config/single_speaker_fast.bin"
ASR_CONFIG="./config/asr_settings_offline.conf"
KEEP_SUCCESS=0
PROFILE="smoke"
WORK_DIR="/tmp/car_terminal_voice_test"

# --- 参数解析 ---
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            [[ $# -ge 2 ]] || { echo "ERROR: --build-dir requires a value" >&2; exit 2; }
            BUILD_DIR="$2"; shift 2 ;;
        --model)
            [[ $# -ge 2 ]] || { echo "ERROR: --model requires a value" >&2; exit 2; }
            MODEL_PATH="$2"; shift 2 ;;
        --config)
            [[ $# -ge 2 ]] || { echo "ERROR: --config requires a value" >&2; exit 2; }
            ASR_CONFIG="$2"; shift 2 ;;
        --keep-success)
            KEEP_SUCCESS=1; shift ;;
        --profile)
            [[ $# -ge 2 ]] || { echo "ERROR: --profile requires a value" >&2; exit 2; }
            PROFILE="$2"; shift 2 ;;
        --help|-h)
            cat <<EOF
Usage: $0 [OPTIONS]

Options:
  --build-dir <path>  Build directory (default: ./build_voice_tools)
  --model <path>      TTS model path (default: ./config/single_speaker_fast.bin)
  --config <path>     ASR config path (default: ./config/asr_settings_offline.conf)
  --profile <name>    Test profile: smoke (default) or accuracy
                      smoke:    验证 TTS->WAV->sherpa->CabinCommand 技术链路
                      accuracy: 保留原始自然指令，记录模型识别准确率
  --keep-success      Keep successful WAV samples (default: only keep failures)
  --help              Show this help
EOF
            exit 0 ;;
        *)
            echo "ERROR: unknown argument: $1" >&2; exit 2 ;;
    esac
done

# --- profile 验证 ---
if [[ "${PROFILE}" != "smoke" && "${PROFILE}" != "accuracy" ]]; then
    echo "ERROR: invalid profile '${PROFILE}', must be 'smoke' or 'accuracy'" >&2
    exit 2
fi

# --- 环境变量：确保终端 UTF-8 输出 ---
export LANG=C.UTF-8
export LC_ALL=C.UTF-8

# --- 依赖检查 ---
TTS_BIN="${BUILD_DIR}/tts_wav_test"
ASR_BIN="${BUILD_DIR}/asr_wav_test"

if [[ ! -x "${TTS_BIN}" ]]; then
    echo "ERROR: tts_wav_test not found or not executable: ${TTS_BIN}" >&2
    echo "       build first: cmake -S voice -B ${BUILD_DIR} && cmake --build ${BUILD_DIR} -j\$(nproc)" >&2
    exit 2
fi
if [[ ! -x "${ASR_BIN}" ]]; then
    echo "ERROR: asr_wav_test not found or not executable: ${ASR_BIN}" >&2
    exit 2
fi
if [[ ! -f "${MODEL_PATH}" ]]; then
    echo "ERROR: TTS model not found: ${MODEL_PATH}" >&2
    exit 2
fi
if [[ ! -f "${ASR_CONFIG}" ]]; then
    echo "ERROR: ASR config not found: ${ASR_CONFIG}" >&2
    exit 2
fi

# --- 测试用例数组 ---
# profile=smoke：使用 ASR 能稳定识别的同义短语，验证 TTS->WAV->sherpa->CabinCommand 技术链路。
# profile=accuracy：保留原始自然指令，用于记录模型识别准确率（允许部分用例失败）。
if [[ "${PROFILE}" == "smoke" ]]; then
    # smoke 用例：ASR 能稳定识别的同义短语
    TEXTS=( "查看地图" "地图放大" "查看摄像头" "播放音乐" "下一首" "查看天气" )
    EXPECT_CMDS=( "MapShow" "MapZoomIn" "MonitorShow" "MusicPlay" "MusicNext" "WeatherShow" )
else
    # accuracy 用例：原始自然指令（"打开地图"/"打开摄像头" 允许失败）
    TEXTS=( "打开地图" "地图放大" "打开摄像头" "播放音乐" "下一首" "查看天气" )
    EXPECT_CMDS=( "MapShow" "MapZoomIn" "MonitorShow" "MusicPlay" "MusicNext" "WeatherShow" )
fi

# --- 工作目录 ---
mkdir -p "${WORK_DIR}"

TOTAL=${#TEXTS[@]}
PASSED=0
FAILED=0
FAIL_DETAILS=()

echo "=========================================="
echo " TTS -> WAV -> ASR -> CabinCommand 验证"
echo "=========================================="
echo "PROFILE      = ${PROFILE}"
echo "BUILD_DIR   = ${BUILD_DIR}"
echo "MODEL_PATH  = ${MODEL_PATH}"
echo "ASR_CONFIG  = ${ASR_CONFIG}"
echo "WORK_DIR    = ${WORK_DIR}"
echo "KEEP_SUCCESS= ${KEEP_SUCCESS}"
echo "TOTAL       = ${TOTAL}"
echo "=========================================="

for (( i=0; i<TOTAL; ++i )); do
    text="${TEXTS[$i]}"
    expect="${EXPECT_CMDS[$i]}"
    # 生成安全的文件名（中文转下标）
    idx=$(printf "%02d" $((i+1)))
    wav_file="${WORK_DIR}/test_${idx}.wav"
    tts_log="${WORK_DIR}/test_${idx}.tts.log"
    asr_log="${WORK_DIR}/test_${idx}.asr.log"

    echo ""
    echo "[$(( i+1 ))/${TOTAL}] text='${text}' expect='${expect}'"

    # --- 阶段1：TTS 合成 WAV ---
    if ! "${TTS_BIN}" \
            --model "${MODEL_PATH}" \
            --text "${text}" \
            --output "${wav_file}" \
            > "${tts_log}" 2>&1; then
        echo "  [FAIL] TTS synthesis failed (see ${tts_log})"
        FAILED=$(( FAILED+1 ))
        FAIL_DETAILS+=("TTS失败: ${text}")
        continue
    fi

    # 检查 TTS 结果
    if ! grep -q "^TTS_RESULT=PASS$" "${tts_log}"; then
        echo "  [FAIL] TTS_RESULT != PASS (see ${tts_log})"
        FAILED=$(( FAILED+1 ))
        FAIL_DETAILS+=("TTS结果异常: ${text}")
        continue
    fi

    # 检查 WAV 文件存在且非空
    if [[ ! -s "${wav_file}" ]]; then
        echo "  [FAIL] WAV file empty or missing: ${wav_file}"
        FAILED=$(( FAILED+1 ))
        FAIL_DETAILS+=("WAV空文件: ${text}")
        continue
    fi

    echo "  TTS PASS, WAV: $(ls -lh "${wav_file}" | awk '{print $5}')"

    # --- 阶段2：ASR 识别 + 指令解析 ---
    if ! "${ASR_BIN}" \
            --config "${ASR_CONFIG}" \
            --wav "${wav_file}" \
            --expect-command "${expect}" \
            > "${asr_log}" 2>&1; then
        # asr_wav_test 退出非0
        asr_text=$(grep "^ASR_TEXT=" "${asr_log}" | head -1 | cut -d= -f2-)
        asr_cmd=$(grep "^COMMAND=" "${asr_log}" | head -1 | cut -d= -f2-)
        echo "  [FAIL] ${text} -> ASR='${asr_text}' CMD='${asr_cmd}' (expected ${expect})"
        echo "         see ${asr_log}"
        FAILED=$(( FAILED+1 ))
        FAIL_DETAILS+=("ASR失败: ${text} -> '${asr_text}' / '${asr_cmd}' (expect ${expect})")
        # 失败样本保留
        continue
    fi

    # 检查 ASR 结果
    if ! grep -q "^ASR_RESULT=PASS$" "${asr_log}"; then
        echo "  [FAIL] ASR_RESULT != PASS (see ${asr_log})"
        FAILED=$(( FAILED+1 ))
        FAIL_DETAILS+=("ASR结果异常: ${text}")
        continue
    fi

    asr_text=$(grep "^ASR_TEXT=" "${asr_log}" | head -1 | cut -d= -f2-)
    asr_cmd=$(grep "^COMMAND=" "${asr_log}" | head -1 | cut -d= -f2-)
    echo "  [PASS] ${text} -> ASR='${asr_text}' CMD='${asr_cmd}'"
    PASSED=$(( PASSED+1 ))

    # 成功样本按参数决定是否保留
    if [[ "${KEEP_SUCCESS}" -eq 0 ]]; then
        rm -f "${wav_file}" "${tts_log}"
        # asr_log 保留作为参考，或在全通过时清理
    fi
done

# --- 汇总 ---
echo ""
echo "=========================================="
echo "PROFILE     = ${PROFILE}"
echo "TOTAL       = ${TOTAL}"
echo "PASSED      = ${PASSED}"
echo "FAILED      = ${FAILED}"
if [[ ${FAILED} -eq 0 ]]; then
    echo "RESULT      = PASS"
    echo "=========================================="
    # 全通过时清理成功样本的日志（如果未指定 --keep-success）
    if [[ "${KEEP_SUCCESS}" -eq 0 ]]; then
        rm -f "${WORK_DIR}"/test_*.tts.log "${WORK_DIR}"/test_*.asr.log 2>/dev/null || true
    fi
    exit 0
else
    echo "RESULT      = FAIL"
    echo "=========================================="
    echo "失败详情："
    for d in "${FAIL_DETAILS[@]}"; do
        echo "  - ${d}"
    done
    echo ""
    echo "失败样本保留在: ${WORK_DIR}/"
    exit 1
fi
