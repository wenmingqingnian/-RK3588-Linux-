#!/bin/bash
# check_dev_env.sh
#
# 开发环境前置检查脚本（环境层）：在开发前 / 新克隆后 / 重置环境后运行。
# 检测 docs/PITFALLS.md 中记录的所有已知陷阱是否已修复。
#
# 与 scripts/verify_no_peripheral.sh 互补：
#   - 本脚本：环境前置检查（开发前）
#   - verify_no_peripheral.sh：功能验收（开发后）
#
# 本脚本不构建任何二进制，不运行 CTest，不依赖麦克风/扬声器/摄像头/NPU。
# 只做静态环境检查 + 配置文件检查 + 板端硬件状态检查。
#
# Usage:
#   ./scripts/check_dev_env.sh                  # 全部检查
#   ./scripts/check_dev_env.sh --strict         # 任何 WARN 也算 FAIL
#   ./scripts/check_dev_env.sh --skip-board     # 跳过需要板端硬件的检查（amixer/NPU/proc/asound）
#   ./scripts/check_dev_env.sh --help
#
# 退出码：
#   0 全部通过（或仅 WARN，且未 --strict）
#   1 至少一个 FAIL
#   2 参数错误或脚本自身错误

set -uo pipefail

# ============================================================
# 默认参数
# ============================================================
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CONFIG_DIR="$PROJECT_DIR/config"
VOICE_DIR="$PROJECT_DIR/voice"
ASR_CONF="$CONFIG_DIR/asr_settings_offline.conf"
VOICE_CONF="$CONFIG_DIR/voice_settings.conf"
HOTWORDS_FILE="$CONFIG_DIR/asr_hotwords.txt"
VM_FILE="$VOICE_DIR/VoiceManager.cpp"
PRO_FILE="$PROJECT_DIR/CarTerminal.pro"

STRICT=0
SKIP_BOARD=0

# 检查计数
TOTAL=0
PASS_CNT=0
WARN_CNT=0
FAIL_CNT=0

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
        --strict)      STRICT=1; shift ;;
        --skip-board)  SKIP_BOARD=1; shift ;;
        --help|-h)
            cat <<EOF
Usage: $0 [OPTIONS]

Options:
  --strict       Treat WARN as FAIL (exit 1 if any WARN)
  --skip-board   Skip checks that need board hardware (amixer/NPU/proc/asound)
  --help         Show this help

Exit codes:
  0  all checks passed (or only WARN, without --strict)
  1  at least one check failed
  2  argument error
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
log_section() { echo -e "\n${BLUE}=== $1 ===${NC}"; }

# 记录检查结果。$1=编号 $2=PASS|WARN|FAIL $3=detail $4=修复提示
record() {
    TOTAL=$((TOTAL+1))
    case "$2" in
        PASS) PASS_CNT=$((PASS_CNT+1));;
        WARN) WARN_CNT=$((WARN_CNT+1));;
        FAIL) FAIL_CNT=$((FAIL_CNT+1));;
    esac
    printf "  [%s] %-6s %s%s\n" "$2" "$1" "$3" "${4:+  -> fix: $4}"
}

# 从 ini 风格配置文件读取键值。$1=file $2=key
get_conf() {
    grep -E "^${2}=" "$1" 2>/dev/null | head -1 | cut -d= -f2-
}

# ============================================================
# 检查函数（与 docs/PITFALLS.md 一一对应）
# ============================================================

# P001 — qmake vs CMake 混用
check_qmake_build_system() {
    log_section "P001: qmake build system"
    if [[ ! -f "$PRO_FILE" ]]; then
        record "P001" FAIL "CarTerminal.pro not found at $PRO_FILE" \
            "git checkout CarTerminal.pro"
        return
    fi
    if ! grep -qE "^TEMPLATE\s*=\s*app" "$PRO_FILE"; then
        record "P001" FAIL "CarTerminal.pro missing TEMPLATE=app" \
            "check $PRO_FILE"
        return
    fi
    if ! grep -qE "^TARGET\s*=\s*CarTerminal" "$PRO_FILE"; then
        record "P001" FAIL "CarTerminal.pro missing TARGET=CarTerminal" \
            "check $PRO_FILE"
        return
    fi
    record "P001" PASS "qmake project file OK"
}

# P002 — 系统时间未同步（未来 mtime）
check_system_time_sync() {
    log_section "P002: system time sync"
    now_ts=$(date +%s)
    if [[ -z "$now_ts" ]]; then
        record "P002" FAIL "cannot get current time"
        return
    fi
    # 检查当前年份是否合理（>= 2024）
    year=$(date +%Y 2>/dev/null | head -1 | tr -d '[:space:]')
    if [[ ! "$year" =~ ^[0-9]+$ ]] || [[ $year -lt 2024 ]]; then
        record "P002" FAIL "system time is wrong (year=$year)" \
            "sudo date -s 'YYYY-MM-DD HH:MM:SS' or ntpdate ntp.aliyun.com"
        return
    fi
    # 检查项目内是否有未来 mtime
    newest_ts=$(find "$PROJECT_DIR" -maxdepth 3 -type f \
                \( -name "*.cpp" -o -name "*.h" -o -name "*.pro" \) \
                -exec stat -c "%Y" {} \; 2>/dev/null | sort -rn | head -1)
    if [[ -n "$newest_ts" && "$newest_ts" =~ ^[0-9]+$ ]]; then
        diff=$((newest_ts - now_ts))
        if [[ $diff -gt 5 ]]; then
            record "P002" FAIL "file mtime is in the future (newest=$newest_ts now=$now_ts, diff=${diff}s)" \
                "find $PROJECT_DIR -type f \\( -name '*.cpp' -o -name '*.h' \\) -exec touch {} \\;"
            return
        fi
    fi
    record "P002" PASS "system time OK (year=$year)"
}

# P003 — ALSA 设备选错（plughw:2,0 vs 3,0）
check_alsa_device() {
    log_section "P003: ALSA device (plughw:3,0)"
    if [[ ! -f "$ASR_CONF" ]]; then
        record "P003" FAIL "ASR config not found: $ASR_CONF"
        return
    fi
    val=$(get_conf "$ASR_CONF" "asr_alsa_device")
    if [[ "$val" == "plughw:3,0" ]]; then
        record "P003" PASS "asr_alsa_device=plughw:3,0"
    else
        record "P003" FAIL "asr_alsa_device='$val' (expected plughw:3,0)" \
            "sed -i 's|^asr_alsa_device=.*|asr_alsa_device=plughw:3,0|' $ASR_CONF"
    fi
}

# P004 — USB 麦克风 Capture Switch 未启用
# P005 — Mic 音量 100% 导致 clipping
check_mic_hardware() {
    log_section "P004+P005: USB microphone hardware"
    if [[ $SKIP_BOARD -eq 1 ]]; then
        record "P004" WARN "skipped (--skip-board)" "run without --skip-board on board"
        record "P005" WARN "skipped (--skip-board)" "run without --skip-board on board"
        return
    fi
    if ! command -v amixer &> /dev/null; then
        record "P004" WARN "amixer not available" "sudo apt-get install alsa-utils"
        record "P005" WARN "amixer not available"
        return
    fi
    # 检查 card 3 是否存在
    if ! amixer -c 3 info &> /dev/null; then
        record "P004" FAIL "USB microphone (card 3) not found" \
            "arecord -l to list cards; check USB connection"
        record "P005" FAIL "card 3 not available"
        return
    fi
    # P004: Capture Switch 是否启用
    cap_val=$(amixer -c 3 contents 2>/dev/null | grep -A2 "Mic Capture Switch" | grep "values" | head -1)
    if echo "$cap_val" | grep -q "on"; then
        record "P004" PASS "Mic Capture Switch enabled"
    elif [[ -z "$cap_val" ]]; then
        # 某些 amixer 不显示 "Switch" 字样，尝试另一种检测
        cap_val2=$(amixer -c 3 sget 'Mic' 2>/dev/null | grep -E "Capture\s+.*values=on")
        if [[ -n "$cap_val2" ]]; then
            record "P004" PASS "Mic capture enabled (via sget)"
        else
            record "P004" FAIL "Mic Capture Switch not enabled" \
                "amixer -c 3 sset 'Mic' 35% cap"
        fi
    else
        record "P004" FAIL "Mic Capture Switch is off" \
            "amixer -c 3 sset 'Mic' 35% cap"
    fi
    # P005: 音量 30-50%
    vol_raw=$(amixer -c 3 sget 'Mic' 2>/dev/null | grep -oE "[0-9]+%" | head -1 | tr -d '%')
    if [[ "$vol_raw" =~ ^[0-9]+$ ]]; then
        if [[ $vol_raw -ge 30 && $vol_raw -le 50 ]]; then
            record "P005" PASS "Mic volume ${vol_raw}% (in 30-50 safe range)"
        elif [[ $vol_raw -gt 50 ]]; then
            record "P005" FAIL "Mic volume ${vol_raw}% too high (clipping risk)" \
                "amixer -c 3 sset 'Mic' 35% cap"
        else
            record "P005" WARN "Mic volume ${vol_raw}% too low (<30%)" \
                "amixer -c 3 sset 'Mic' 35% cap"
        fi
    else
        record "P005" WARN "cannot parse Mic volume" "amixer -c 3 sget 'Mic'"
    fi
}

# P006 — VoiceManager 用 snd_pcm_start 而非 priming readi
check_priming_readi() {
    log_section "P006: VoiceManager priming readi"
    if [[ ! -f "$VM_FILE" ]]; then
        record "P006" WARN "VoiceManager.cpp not found at $VM_FILE"
        return
    fi
    if ! grep -q "ALSA stream primed" "$VM_FILE"; then
        record "P006" FAIL "priming readi pattern not found" \
            "see docs/PITFALLS.md P006 for code template"
        return
    fi
    if grep -qE "snd_pcm_start\s*\(\s*m_alsaHandle" "$VM_FILE"; then
        record "P006" FAIL "snd_pcm_start(m_alsaHandle) is present (must use readi)" \
            "remove snd_pcm_start; use snd_pcm_readi for priming"
        return
    fi
    record "P006" PASS "VoiceManager uses priming snd_pcm_readi"
}

# P007-P009 — ASR 时序（文档提示）
check_asr_timing_docs() {
    log_section "P007+P008+P009: ASR timing reminders"
    if [[ ! -f "$PROJECT_DIR/docs/PITFALLS.md" ]]; then
        record "P007" WARN "docs/PITFALLS.md missing (timing docs absent)"
        return
    fi
    if grep -q "6 seconds" "$PROJECT_DIR/docs/PITFALLS.md" && \
       grep -q "3 seconds" "$PROJECT_DIR/docs/PITFALLS.md" && \
       grep -q "2.4 seconds" "$PROJECT_DIR/docs/PITFALLS.md"; then
        record "P007" PASS "ASR timing docs present (6s startup / 3s post-cmd / 2.4s tail)"
        record "P008" PASS "(see P007 — covered by same docs)"
        record "P009" PASS "(see P007 — covered by same docs)"
    else
        record "P007" WARN "ASR timing docs incomplete in docs/PITFALLS.md"
    fi
}

# P010 — sherpa 版本/SHA 不匹配
check_sherpa_version() {
    log_section "P010: sherpa-onnx version"
    if [[ $SKIP_BOARD -eq 1 ]]; then
        record "P010" WARN "skipped (--skip-board)"
        return
    fi
    sherpa_lib="/usr/lib/aarch64-linux-gnu/libsherpa-onnx-c-api.so"
    if [[ ! -f "$sherpa_lib" ]]; then
        record "P010" WARN "sherpa lib not found at $sherpa_lib" \
            "sudo apt-get install libsherpa-onnx-dev"
        return
    fi
    # SHA256 校验
    expected_sha="c713c6c6ca7fcaff01607768d3a163f73aae0c83e476bc1bcbce7839b9695507"
    actual_sha=$(sha256sum "$sherpa_lib" | awk '{print $1}')
    if [[ "$actual_sha" == "$expected_sha" ]]; then
        record "P010" PASS "sherpa lib SHA256 matches 1.13.3/330609da"
    else
        record "P010" FAIL "sherpa lib SHA256 mismatch (got $actual_sha)" \
            "install sherpa-onnx 1.13.3 with SHA 330609da"
    fi
}

# P011 — 解码方法 ≠ modified_beam_search
check_decoding_method() {
    log_section "P011: decoding method"
    val=$(get_conf "$ASR_CONF" "asr_decoding_method")
    if [[ "$val" == "modified_beam_search" ]]; then
        record "P011" PASS "asr_decoding_method=modified_beam_search"
    else
        record "P011" FAIL "asr_decoding_method='$val'" \
            "sed -i 's|^asr_decoding_method=.*|asr_decoding_method=modified_beam_search|' $ASR_CONF"
    fi
}

# P012 — 热词 score ≠ 2.5
check_hotwords_score() {
    log_section "P012: hotwords score"
    val=$(get_conf "$ASR_CONF" "asr_hotwords_score")
    if [[ "$val" == "2.5" ]]; then
        record "P012" PASS "asr_hotwords_score=2.5"
    else
        record "P012" FAIL "asr_hotwords_score='$val'" \
            "sed -i 's|^asr_hotwords_score=.*|asr_hotwords_score=2.5|' $ASR_CONF"
    fi
}

# P013 — asr_hotwords.txt 缺失
check_hotwords_file() {
    log_section "P013: asr_hotwords.txt"
    if [[ ! -f "$HOTWORDS_FILE" ]]; then
        record "P013" FAIL "asr_hotwords.txt not found" \
            "cp config/asr_hotwords.example.txt config/asr_hotwords.txt"
        return
    fi
    if [[ ! -s "$HOTWORDS_FILE" ]]; then
        record "P013" FAIL "asr_hotwords.txt is empty" \
            "add at least one hotword phrase"
        return
    fi
    # BOM 检测（前 3 字节 EF BB BF）
    bom=$(head -c 3 "$HOTWORDS_FILE" | xxd -p 2>/dev/null | head -1)
    if [[ "$bom" == "efbbbf" ]]; then
        record "P013" FAIL "asr_hotwords.txt has UTF-8 BOM" \
            "sed -i '1s/^\\xEF\\xBB\\xBF//' $HOTWORDS_FILE"
        return
    fi
    record "P013" PASS "asr_hotwords.txt OK (size=$(stat -c %s "$HOTWORDS_FILE") bytes)"
}

# P014 — 测试时 TTS 反馈未关闭（仅 WARN，因为正常使用要 true）
check_tts_feedback_for_testing() {
    log_section "P014: TTS feedback (testing reminder)"
    val=$(get_conf "$VOICE_CONF" "tts_feedback_for_command")
    if [[ "$val" == "true" ]]; then
        record "P014" PASS "tts_feedback_for_command=true (normal mode)"
        echo -e "  ${YELLOW}NOTE${NC}: set to false when testing ASR accuracy to avoid echo loop"
    elif [[ "$val" == "false" ]]; then
        record "P014" WARN "tts_feedback_for_command=false (test mode)" \
            "set back to true for normal use"
    else
        record "P014" WARN "tts_feedback_for_command='$val' (expected true/false)"
    fi
}

# P015 — ZMQ 用 localhost 而非 127.0.0.1
check_zmq_address() {
    log_section "P015: ZMQ address (127.0.0.1)"
    if [[ ! -f "$VOICE_CONF" ]]; then
        record "P015" FAIL "voice config not found: $VOICE_CONF"
        return
    fi
    bad=$(grep -E "^(llm_address|tts_address|tts_status_address)=" "$VOICE_CONF" \
          | grep -c "localhost" | head -1 | tr -d '[:space:]')
    bad=${bad:-0}
    if [[ "$bad" =~ ^[0-9]+$ ]] && [[ $bad -eq 0 ]]; then
        record "P015" PASS "ZMQ addresses use 127.0.0.1"
    else
        record "P015" FAIL "$bad ZMQ address(es) still use localhost" \
            "sed -i 's|tcp://localhost:|tcp://127.0.0.1:|g' $VOICE_CONF"
    fi
}

# P016 — /etc/hosts 缺少 localhost 条目
check_etc_hosts() {
    log_section "P016: /etc/hosts localhost entry"
    if [[ ! -f /etc/hosts ]]; then
        record "P016" WARN "/etc/hosts not readable"
        return
    fi
    if grep -qE "^127\.0\.0\.1\s+localhost" /etc/hosts; then
        record "P016" PASS "/etc/hosts has 127.0.0.1 localhost"
    else
        record "P016" FAIL "/etc/hosts missing '127.0.0.1 localhost'" \
            "echo '127.0.0.1 localhost' | sudo tee -a /etc/hosts"
    fi
}

# P017 — TTS mute window 配置错误
check_tts_mute_window() {
    log_section "P017: TTS mute window"
    val=$(get_conf "$VOICE_CONF" "tts_feedback_mute_ms")
    if [[ "$val" == "3000" ]]; then
        record "P017" PASS "tts_feedback_mute_ms=3000"
    elif [[ "$val" == "0" ]]; then
        record "P017" FAIL "tts_feedback_mute_ms=0 (echo loop risk)" \
            "sed -i 's|^tts_feedback_mute_ms=.*|tts_feedback_mute_ms=3000|' $VOICE_CONF"
    elif [[ "$val" =~ ^[0-9]+$ ]] && [[ $val -gt 10000 ]]; then
        record "P017" FAIL "tts_feedback_mute_ms=$val too long (>10s)" \
            "sed -i 's|^tts_feedback_mute_ms=.*|tts_feedback_mute_ms=3000|' $VOICE_CONF"
    else
        record "P017" WARN "tts_feedback_mute_ms='$val' (expected 3000)"
    fi
}

# P018 — llm_enabled=true 但 NPU 驱动 <v0.9.7
check_llm_disabled() {
    log_section "P018: LLM disabled (NPU driver guard)"
    val=$(get_conf "$VOICE_CONF" "llm_enabled")
    if [[ "$val" == "false" ]]; then
        record "P018" PASS "llm_enabled=false"
        return
    fi
    if [[ "$val" == "true" ]]; then
        # 检查 NPU 驱动版本
        if [[ $SKIP_BOARD -eq 1 ]]; then
            record "P018" WARN "llm_enabled=true but NPU driver not checked (--skip-board)" \
                "set llm_enabled=false or run without --skip-board"
            return
        fi
        npu_ver_file="/sys/kernel/debug/rknpu/version"
        if [[ -f "$npu_ver_file" ]]; then
            npu_ver=$(sudo cat "$npu_ver_file" 2>/dev/null | grep -oE "v[0-9]+\.[0-9]+\.[0-9]+" | head -1)
            if [[ "$npu_ver" == "v0.9.7" || "$npu_ver" == "v0.9.8" || "$npu_ver" =~ ^v0\.9\.([7-9]|[1-9][0-9])$ ]]; then
                record "P018" PASS "NPU driver $npu_ver >= v0.9.7, llm_enabled=true OK"
            else
                record "P018" FAIL "llm_enabled=true but NPU driver=$npu_ver < v0.9.7 (SIGBUS risk)" \
                    "sed -i 's|^llm_enabled=.*|llm_enabled=false|' $VOICE_CONF"
            fi
        else
            record "P018" FAIL "llm_enabled=true but cannot read NPU driver version" \
                "sed -i 's|^llm_enabled=.*|llm_enabled=false|' $VOICE_CONF"
        fi
    else
        record "P018" WARN "llm_enabled='$val' (expected true/false)"
    fi
}

# P019 — ASR_DBG 调试代码残留
check_asr_dbg_clean() {
    log_section "P019: ASR_DBG cleanup"
    if [[ ! -f "$VM_FILE" ]]; then
        record "P019" WARN "VoiceManager.cpp not found"
        return
    fi
    cnt=$(grep -c "ASR_DBG" "$VM_FILE" 2>/dev/null | head -1 | tr -d '[:space:]')
    cnt=${cnt:-0}
    if [[ "$cnt" =~ ^[0-9]+$ ]] && [[ $cnt -eq 0 ]]; then
        record "P019" PASS "no ASR_DBG residue"
    else
        record "P019" FAIL "ASR_DBG residue: count=$cnt" \
            "remove all ASR_DBG lines from $VM_FILE"
    fi
}

# ============================================================
# 主流程
# ============================================================
echo -e "${BLUE}============================================================${NC}"
echo -e "${BLUE} RK3588S_CarTerminal — Development Environment Check${NC}"
echo -e "${BLUE} Project: $PROJECT_DIR${NC}"
echo -e "${BLUE} Date:    $(date)${NC}"
echo -e "${BLUE}============================================================${NC}"

# 前置依赖检查
log_section "Pre-flight: required files"
if [[ ! -f "$ASR_CONF" ]];   then log_error "missing $ASR_CONF";   exit 2; fi
if [[ ! -f "$VOICE_CONF" ]]; then log_error "missing $VOICE_CONF"; exit 2; fi
log_info "required config files OK"

# 运行所有检查
check_qmake_build_system
check_system_time_sync
check_alsa_device
check_mic_hardware
check_priming_readi
check_asr_timing_docs
check_sherpa_version
check_decoding_method
check_hotwords_score
check_hotwords_file
check_tts_feedback_for_testing
check_zmq_address
check_etc_hosts
check_tts_mute_window
check_llm_disabled
check_asr_dbg_clean

# ============================================================
# 汇总
# ============================================================
echo -e "\n${BLUE}============================================================${NC}"
echo -e "${BLUE} Summary${NC}"
echo -e "${BLUE}============================================================${NC}"
printf "  Total: %d\n" $TOTAL
printf "  ${GREEN}PASS${NC}:  %d\n" $PASS_CNT
printf "  ${YELLOW}WARN${NC}:  %d\n" $WARN_CNT
printf "  ${RED}FAIL${NC}:  %d\n" $FAIL_CNT

if [[ $FAIL_CNT -gt 0 ]]; then
    echo -e "\n${RED}RESULT = FAIL${NC} (see fixes above)"
    exit 1
fi

if [[ $STRICT -eq 1 && $WARN_CNT -gt 0 ]]; then
    echo -e "\n${YELLOW}RESULT = FAIL${NC} (--strict: WARN counts as FAIL)"
    exit 1
fi

if [[ $WARN_CNT -gt 0 ]]; then
    echo -e "\n${GREEN}RESULT = PASS${NC} with ${WARN_CNT} WARN (use --strict to fail on WARN)"
else
    echo -e "\n${GREEN}RESULT = PASS${NC} (all checks clean)"
fi
exit 0
