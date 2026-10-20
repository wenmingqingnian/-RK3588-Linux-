#!/bin/bash
# verify_no_peripheral.sh
#
# 一键无外设验收脚本（无麦克风/无扬声器/无摄像头/无 NPU）。
# 整合红线校验 + CTest + TTS→WAV→ASR 闭环 + 主程序构建 + (可选) GUI 注入。
#
# 任一阶段失败必须返回非0；不得把"0个测试"当成通过。
# GUI 注入作为可选阶段：无 DISPLAY 时标记 SKIP，不影响纯命令行验收。
#
# Usage:
#   ./scripts/verify_no_peripheral.sh                 # 全流程（除 GUI 注入自动 SKIP）
#   ./scripts/verify_no_peripheral.sh --with-gui      # 强制启用 GUI 注入测试
#   ./scripts/verify_no_peripheral.sh --skip-voice    # 跳过 voice/ 子目录的 CTest 与 WAV 闭环
#   ./scripts/verify_no_peripheral.sh --skip-build    # 跳过主程序 qmake+make（仅做测试）
#   ./scripts/verify_no_peripheral.sh --help
#
# 退出码：
#   0 全部通过
#   1 至少一个阶段失败
#   2 参数错误或依赖缺失

set -uo pipefail

# ============================================================
# 默认参数
# ============================================================
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CONFIG_DIR="$PROJECT_DIR/config"
VOICE_DIR="$PROJECT_DIR/voice"
ASR_CONF="$CONFIG_DIR/asr_settings_offline.conf"
VOICE_CONF="$CONFIG_DIR/voice_settings.conf"

VOICE_BUILD_DIR="$PROJECT_DIR/build_voice_tools"
VOICE_WAV_SCRIPT="$VOICE_DIR/scripts/verify_tts_asr_wav.sh"
TTS_MODEL="$CONFIG_DIR/single_speaker_fast.bin"

WITH_GUI=0
SKIP_VOICE=0
SKIP_BUILD=0

# 阶段计数
STAGES_TOTAL=0
STAGES_PASS=0
STAGES_FAIL=0
STAGES_SKIP=0

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# ============================================================
# 参数解析
# ============================================================
while [[ $# -gt 0 ]]; do
    case "$1" in
        --with-gui)    WITH_GUI=1; shift ;;
        --skip-voice)  SKIP_VOICE=1; shift ;;
        --skip-build)  SKIP_BUILD=1; shift ;;
        --help|-h)
            cat <<EOF
Usage: $0 [OPTIONS]

Options:
  --with-gui      Force enable GUI text-injection test (default: SKIP if no DISPLAY)
  --skip-voice    Skip voice/ CTest and TTS->WAV->ASR closed loop
  --skip-build    Skip main program qmake+make (run tests only)
  --help          Show this help

Exit codes:
  0  all stages passed
  1  at least one stage failed
  2  argument error or missing dependency
EOF
            exit 0 ;;
        *) echo "ERROR: unknown argument: $1" >&2; exit 2 ;;
    esac
done

# ============================================================
# 工具函数
# ============================================================
log_info()  { echo -e "${GREEN}[INFO]${NC}  $1"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_stage() { echo -e "\n${BLUE}=== Stage: $1 ===${NC}"; }

# 阶段结果登记。$1=stage名 $2=PASS|FAIL|SKIP $3=detail
record_stage() {
    STAGES_TOTAL=$((STAGES_TOTAL+1))
    case "$2" in
        PASS) STAGES_PASS=$((STAGES_PASS+1));;
        FAIL) STAGES_FAIL=$((STAGES_FAIL+1));;
        SKIP) STAGES_SKIP=$((STAGES_SKIP+1));;
    esac
    printf "  -> [%s] %s%s\n" "$2" "$1" "${3:+ ($3)}"
}

# 期望非空文件。$1=path $2=label
require_file() {
    if [[ ! -s "$1" ]]; then
        log_error "$2 not found or empty: $1"
        exit 2
    fi
}

# ============================================================
# 前置依赖检查
# ============================================================
log_stage "Pre-flight: dependencies"

require_file "$ASR_CONF"   "ASR config"
require_file "$VOICE_CONF" "voice config"

if ! command -v qmake &> /dev/null; then
    log_error "qmake not found in PATH"
    exit 2
fi

if [[ $SKIP_VOICE -eq 0 ]]; then
    require_file "$TTS_MODEL"       "TTS model"
    require_file "$VOICE_WAV_SCRIPT" "verify_tts_asr_wav.sh"
    if ! command -v cmake &> /dev/null; then
        log_error "cmake not found (required for voice/ CTest and tools)"
        exit 2
    fi
fi

log_info "pre-flight OK"
record_stage "pre-flight" PASS

# ============================================================
# Stage 1: 红线配置校验（5 条 do-not-regress）
# ============================================================
log_stage "Stage 1: red-line config verification"

REDLINE_FAIL=0

# 红线 1: asr_alsa_device=plughw:3,0
val=$(grep -E "^asr_alsa_device=" "$ASR_CONF" | head -1 | cut -d= -f2-)
if [[ "$val" == "plughw:3,0" ]]; then
    log_info "redline 1 ASR device = plughw:3,0"
else
    log_error "redline 1 violated: asr_alsa_device='$val' (expected plughw:3,0)"
    REDLINE_FAIL=1
fi

# 红线 2: ZMQ 地址必须是 127.0.0.1（不能是 localhost）
zmq_bad=$(grep -E "^(llm_address|tts_address|tts_status_address)=" "$VOICE_CONF" | grep -c "localhost" | head -1 | tr -d '[:space:]')
zmq_bad=${zmq_bad:-0}
if [[ "$zmq_bad" =~ ^[0-9]+$ ]] && [[ $zmq_bad -eq 0 ]]; then
    log_info "redline 2 ZMQ addresses use 127.0.0.1"
else
    log_error "redline 2 violated: $zmq_bad ZMQ address(es) still use localhost"
    REDLINE_FAIL=1
fi

# 红线 3: tts_feedback_for_command=true 且 tts_feedback_mute_ms=3000
v1=$(grep -E "^tts_feedback_for_command=" "$VOICE_CONF" | head -1 | cut -d= -f2-)
v2=$(grep -E "^tts_feedback_mute_ms=" "$VOICE_CONF" | head -1 | cut -d= -f2-)
if [[ "$v1" == "true" && "$v2" == "3000" ]]; then
    log_info "redline 3 TTS feedback enabled, mute_ms=3000"
else
    log_error "redline 3 violated: tts_feedback_for_command='$v1' mute_ms='$v2'"
    REDLINE_FAIL=1
fi

# 红线 4: llm_enabled=false
v3=$(grep -E "^llm_enabled=" "$VOICE_CONF" | head -1 | cut -d= -f2-)
if [[ "$v3" == "false" ]]; then
    log_info "redline 4 llm_enabled=false"
else
    log_error "redline 4 violated: llm_enabled='$v3' (RKNPU 0.9.6 SIGBUS risk)"
    REDLINE_FAIL=1
fi

# 红线 5: VoiceManager.cpp 必须用 priming snd_pcm_readi（不能是 snd_pcm_start）
VM="$VOICE_DIR/VoiceManager.cpp"
if [[ -f "$VM" ]]; then
    # 禁止在 priming 上下文用 snd_pcm_start（允许出现在注释或无关位置，但 priming 必须是 readi）
    if grep -q "ALSA stream primed (first readi" "$VM" \
       && ! grep -qE "snd_pcm_start\s*\(\s*m_alsaHandle" "$VM"; then
        log_info "redline 5 VoiceManager uses priming snd_pcm_readi"
    else
        log_error "redline 5 violated: priming readi pattern not found or snd_pcm_start() present"
        REDLINE_FAIL=1
    fi
else
    log_warn "VoiceManager.cpp not found at $VM, skipping redline 5 code check"
fi

# ASR_DBG 残留检查（工程纪律）
if [[ -f "$VM" ]]; then
    # 注意：grep -c 可能返回多行（如果文件名含特殊字符）或带换行；
    # 用 head -1 + tr 去掉换行，再用正则匹配确保 dbg 是纯数字
    dbg=$(grep -c "ASR_DBG" "$VM" 2>/dev/null | head -1 | tr -d '[:space:]')
    if [[ "$dbg" =~ ^[0-9]+$ ]] && [[ $dbg -eq 0 ]]; then
        log_info "ASR_DBG cleanup OK (count=0)"
    else
        log_error "ASR_DBG residue: count='$dbg'"
        REDLINE_FAIL=1
    fi
fi

# 附加：sherpa 配置一致性
asr_method=$(grep -E "^asr_decoding_method=" "$ASR_CONF" | head -1 | cut -d= -f2-)
asr_score=$(grep -E "^asr_hotwords_score=" "$ASR_CONF" | head -1 | cut -d= -f2-)
if [[ "$asr_method" == "modified_beam_search" && "$asr_score" == "2.5" ]]; then
    log_info "sherpa config OK: method=$asr_method score=$asr_score"
else
    log_warn "sherpa config unusual: method='$asr_method' score='$asr_score' (expected modified_beam_search / 2.5)"
fi

if [[ $REDLINE_FAIL -eq 0 ]]; then
    record_stage "redline-config" PASS
else
    record_stage "redline-config" FAIL "5 redlines or ASR_DBG"
    exit 1
fi

# ============================================================
# Stage 2: 主程序构建（qmake + make）
# ============================================================
if [[ $SKIP_BUILD -eq 1 ]]; then
    log_stage "Stage 2: main program build (SKIPPED)"
    record_stage "main-build" SKIP "--skip-build"
else
    log_stage "Stage 2: main program build (qmake CarTerminal.pro && make -j8)"

    # 时间戳检查：避免"修改时间在未来"导致 make 无限循环
    now_ts=$(date +%s)
    newest_ts=$(find "$PROJECT_DIR" -maxdepth 3 \( -name "*.cpp" -o -name "*.h" -o -name "*.pro" \) \
                -exec stat -c "%Y" {} \; 2>/dev/null | sort -rn | head -1)
    if [[ -n "$newest_ts" && $newest_ts -gt $((now_ts+2)) ]]; then
        log_warn "file mtime is in the future (newest=$newest_ts now=$now_ts), running touch"
        find "$PROJECT_DIR" -maxdepth 3 -type f \( -name "*.cpp" -o -name "*.h" -o -name "*.pro" -o -name "*.ui" -o -name "*.qrc" \) -exec touch {} \;
    fi

    cd "$PROJECT_DIR"
    build_log="/tmp/verify_no_periph_build.log"
    if qmake CarTerminal.pro > "$build_log" 2>&1 && make -j8 >> "$build_log" 2>&1; then
        if [[ -x "$PROJECT_DIR/CarTerminal" ]]; then
            log_info "build OK, binary: $(ls -la "$PROJECT_DIR/CarTerminal" | awk '{print $5, $6, $7, $8}')"
            record_stage "main-build" PASS
        else
            log_error "make succeeded but CarTerminal binary not found"
            record_stage "main-build" FAIL "no binary"
            exit 1
        fi
    else
        log_error "build failed (see $build_log)"
        tail -30 "$build_log" >&2
        record_stage "main-build" FAIL "see $build_log"
        exit 1
    fi
fi

# ============================================================
# Stage 3: voice/ 子目录 CTest（wav_io / asr_abi / vehiclecommand / asr_config）
# ============================================================
if [[ $SKIP_VOICE -eq 1 ]]; then
    log_stage "Stage 3: voice CTest (SKIPPED)"
    record_stage "voice-ctest" SKIP "--skip-voice"
else
    log_stage "Stage 3: voice CTest (wav_io / asr_abi / vehiclecommand / asr_config)"

    # 构建 voice 工具（如果未构建）
    if [[ ! -x "$VOICE_BUILD_DIR/test_wav_io" || ! -x "$VOICE_BUILD_DIR/test_asr_abi" \
          || ! -x "$VOICE_BUILD_DIR/test_vehiclecommand" || ! -x "$VOICE_BUILD_DIR/test_asr_config" \
          || ! -x "$VOICE_BUILD_DIR/tts_wav_test" || ! -x "$VOICE_BUILD_DIR/asr_wav_test" ]]; then
        log_info "building voice tools in $VOICE_BUILD_DIR ..."
        voice_build_log="/tmp/verify_no_periph_voice_build.log"
        if ! cmake -S "$VOICE_DIR" -B "$VOICE_BUILD_DIR" > "$voice_build_log" 2>&1 \
              || ! cmake --build "$VOICE_BUILD_DIR" -j8 >> "$voice_build_log" 2>&1; then
            log_error "voice build failed (see $voice_build_log)"
            tail -30 "$voice_build_log" >&2
            record_stage "voice-ctest" FAIL "voice build"
            exit 1
        fi
    fi

    # 旧版 ctest 不支持 --test-dir，进构建目录执行
    ctest_log="/tmp/verify_no_periph_ctest.log"
    cd "$VOICE_BUILD_DIR"
    # 注意：ctest 默认 exit code 非0即失败；0个测试也会返回0，需额外校验
    ctest > "$ctest_log" 2>&1
    ctest_rc=$?
    ctest_output=$(cat "$ctest_log")

    if [[ $ctest_rc -ne 0 ]]; then
        log_error "ctest failed (rc=$ctest_rc)"
        echo "$ctest_output" | tail -30 >&2
        record_stage "voice-ctest" FAIL "rc=$ctest_rc"
        exit 1
    fi

    # 用 ctest -N 取注册的测试数量（比解析 ctest 输出更可靠）
    ctest_count=$(ctest -N 2>/dev/null | grep -E "^Total Tests:" | head -1 | grep -oE "[0-9]+" | head -1 | tr -d '[:space:]')
    ctest_count=${ctest_count:-0}

    if [[ ! "$ctest_count" =~ ^[0-9]+$ ]] || [[ $ctest_count -eq 0 ]]; then
        log_error "ctest reported 0 tests — treating as FAIL (no tests = no coverage)"
        echo "$ctest_output" | tail -10 >&2
        record_stage "voice-ctest" FAIL "0 tests"
        exit 1
    fi

    # 校验 4 个期望测试都存在
    missing=0
    for t in wav_io_tests asr_abi_tests vehiclecommand_tests asr_config_tests; do
        if ! ctest -N 2>/dev/null | grep -q "Test.*:.*$t"; then
            log_error "expected test not registered: $t"
            missing=$((missing+1))
        fi
    done
    if [[ $missing -gt 0 ]]; then
        record_stage "voice-ctest" FAIL "$missing test(s) missing"
        exit 1
    fi

    log_info "ctest OK: $ctest_count tests passed"
    record_stage "voice-ctest" PASS "$ctest_count tests"
fi

# ============================================================
# Stage 4: TTS→WAV→ASR→CabinCommand 闭环（smoke profile）
# ============================================================
if [[ $SKIP_VOICE -eq 1 ]]; then
    log_stage "Stage 4: TTS→WAV→ASR closed loop (SKIPPED)"
    record_stage "tts-asr-closedloop" SKIP "--skip-voice"
else
    log_stage "Stage 4: TTS→WAV→ASR closed loop (smoke profile)"

    closed_loop_log="/tmp/verify_no_periph_closedloop.log"
    cd "$PROJECT_DIR"
    if bash "$VOICE_WAV_SCRIPT" --profile smoke --build-dir "$VOICE_BUILD_DIR" \
            --model "$TTS_MODEL" --config "$ASR_CONF" > "$closed_loop_log" 2>&1; then
        # verify_tts_asr_wav.sh 的汇总格式是 "PASSED      = 6" / "TOTAL       = 6"
        # 用 awk -F= 按等号分割取第二字段
        pass_count=$(grep -E "^PASSED" "$closed_loop_log" | head -1 | awk -F= '{print $2}' | tr -d '[:space:]')
        total_count=$(grep -E "^TOTAL" "$closed_loop_log" | head -1 | awk -F= '{print $2}' | tr -d '[:space:]')
        pass_count=${pass_count:-0}
        total_count=${total_count:-0}
        if [[ "$pass_count" =~ ^[0-9]+$ ]] && [[ "$total_count" =~ ^[0-9]+$ ]] \
           && [[ "$pass_count" == "$total_count" ]] && [[ $total_count -ge 6 ]]; then
            log_info "closed loop OK: $pass_count/$total_count passed"
            record_stage "tts-asr-closedloop" PASS "$pass_count/$total_count"
        else
            log_error "closed loop result unusual: pass='$pass_count' total='$total_count'"
            tail -20 "$closed_loop_log" >&2
            record_stage "tts-asr-closedloop" FAIL "pass=$pass_count/total=$total_count"
            exit 1
        fi
    else
        log_error "closed loop script failed (see $closed_loop_log)"
        tail -30 "$closed_loop_log" >&2
        record_stage "tts-asr-closedloop" FAIL
        exit 1
    fi
fi

# ============================================================
# Stage 5 (可选): GUI 文本注入测试
# 默认不跑（"无外设验收"语义：GUI/HDMI 算外设）；--with-gui 才跑。
# --with-gui 但无 DISPLAY 时 FAIL（用户明确要求却无法满足）。
# ============================================================
if [[ $WITH_GUI -eq 0 ]]; then
    log_stage "Stage 5: GUI text-injection test (SKIPPED: use --with-gui to enable)"
    record_stage "gui-inject" SKIP "use --with-gui to enable"
elif [[ -z "${DISPLAY:-}" ]]; then
    log_stage "Stage 5: GUI text-injection test"
    log_error "--with-gui requested but DISPLAY is not set"
    record_stage "gui-inject" FAIL "no DISPLAY"
    exit 1
else
    log_stage "Stage 5: GUI text-injection test (VEHICLE_VOICE_TEST_TEXT)"

    # 清理残留
    pkill -9 tts_service 2>/dev/null || true
    pkill -9 CarTerminal 2>/dev/null || true
    sleep 1

    # 端口检查
    if ss -tlnp 2>/dev/null | grep -qE ":(7777|6677)"; then
        log_error "ports 7777/6677 still in use"
        record_stage "gui-inject" FAIL "ports busy"
        exit 1
    fi

    gui_log="/tmp/verify_no_periph_gui.log"
    cd "$PROJECT_DIR"
    rm -f "$gui_log"

    VEHICLE_VOICE_TEST_TEXT="打开地图" ./CarTerminal > "$gui_log" 2>&1 &
    car_pid=$!
    log_info "CarTerminal started pid=$car_pid, waiting 8s for startup + inject + TTS"
    sleep 8

    # 期望日志序列
    expected_patterns=(
        "ASR muted for TTS feedback"
        "sending to TTS: 已为您打开地图END"
        "TTS ack: ok"
        "ASR stream reset"
        "ASR unmuted after TTS feedback window"
    )
    missing_patterns=()
    for p in "${expected_patterns[@]}"; do
        if ! grep -qF "$p" "$gui_log"; then
            missing_patterns+=("$p")
        fi
    done

    # 清理
    kill -9 $car_pid 2>/dev/null || true
    sleep 1
    pkill -9 tts_service 2>/dev/null || true

    if [[ ${#missing_patterns[@]} -eq 0 ]]; then
        log_info "GUI inject OK: all 5 expected log patterns present"
        record_stage "gui-inject" PASS "5/5 patterns"
    else
        log_error "GUI inject missing patterns:"
        for p in "${missing_patterns[@]}"; do
            log_error "  - $p"
        done
        tail -30 "$gui_log" >&2
        record_stage "gui-inject" FAIL "${#missing_patterns[@]} patterns missing"
        exit 1
    fi
fi

# ============================================================
# 汇总
# ============================================================
echo ""
echo "=========================================="
echo " Verification Summary"
echo "=========================================="
echo "Total stages : $STAGES_TOTAL"
echo "  PASS       : $STAGES_PASS"
echo "  FAIL       : $STAGES_FAIL"
echo "  SKIP       : $STAGES_SKIP"
echo "=========================================="

if [[ $STAGES_FAIL -gt 0 ]]; then
    echo "RESULT = FAIL"
    exit 1
elif [[ $STAGES_PASS -eq 0 ]]; then
    echo "RESULT = FAIL (no stages passed)"
    exit 1
else
    echo "RESULT = PASS"
    exit 0
fi
