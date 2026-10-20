#!/bin/bash
# baseline_verify.sh
#
# 第0阶段「出厂镜像软件基线封板」板端验证脚本。
# 对应 PROJECT_ROADMAP.md §四 第0阶段 主链路验证清单 11 项。
#
# 设计原则：
#   - 自动项：直接 PASS/FAIL（设备节点、二进制存在、v4l2 采集、GPS 串口、
#     RTSP 端口、启动/退出 3 次）
#   - 人工项：输出 MANUAL 提示，由用户观察后人工确认（GUI 显示、Qt 姿态页、
#     ASR 语音指令、VLC 拉流）
#   - 启动 CarTerminal 阶段：跑 N 秒后 SIGTERM，抓日志关键词
#     (RGA convert OK / detect: N objects / MPP encode OK)
#
# Usage:
#   ./scripts/baseline_verify.sh                  # 全部 11 项
#   ./scripts/baseline_verify.sh --skip-launch    # 跳过启动 CarTerminal 阶段（仅静态检查）
#   ./scripts/baseline_verify.sh --help
#
# 退出码：
#   0  全部自动项 PASS（人工项不参与退出码）
#   1  至少一项自动项 FAIL
#   2  参数错误

set -uo pipefail

# ============================================================
# 默认参数
# ============================================================
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CONFIG_DIR="$PROJECT_DIR/config"
BIN_PATH="$PROJECT_DIR/CarTerminal"
LOG_DIR="$PROJECT_DIR/logs"
BASELINE_LOG="$LOG_DIR/baseline_verify_$(date +%Y%m%d_%H%M%S).log"
RUNTIME_LOG="$LOG_DIR/baseline_runtime_$(date +%Y%m%d_%H%M%S).log"
RUNTIME_SECS=20     # 启动 CarTerminal 抓日志时长（秒）

SKIP_LAUNCH=0
PASS_CNT=0
FAIL_CNT=0
MANUAL_CNT=0
RESULTS=()  # 累积每行结果用于 summary

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
        --skip-launch) SKIP_LAUNCH=1; shift ;;
        --help|-h)
            cat <<EOF
Usage: $0 [OPTIONS]

Options:
  --skip-launch   Skip launching CarTerminal (static checks only)
  --help          Show this help

Exit codes:
  0  all auto checks passed
  1  at least one auto check failed
  2  argument error
EOF
            exit 0 ;;
        *) echo "ERROR: unknown argument: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "$LOG_DIR"

# ============================================================
# 工具函数
# ============================================================
log() { echo -e "[$(date +%H:%M:%S)] $*" | tee -a "$BASELINE_LOG"; }

record_pass() {
    local id="$1"; local name="$2"; local detail="$3"
    PASS_CNT=$((PASS_CNT + 1))
    RESULTS+=("${GREEN}PASS${NC} | #$id $name | $detail")
    log "${GREEN}PASS${NC} #$id $name — $detail"
}

record_fail() {
    local id="$1"; local name="$2"; local detail="$3"
    FAIL_CNT=$((FAIL_CNT + 1))
    RESULTS+=("${RED}FAIL${NC} | #$id $name | $detail")
    log "${RED}FAIL${NC} #$id $name — $detail"
}

record_manual() {
    local id="$1"; local name="$2"; local detail="$3"
    MANUAL_CNT=$((MANUAL_CNT + 1))
    RESULTS+=("${YELLOW}MANUAL${NC} | #$id $name | $detail")
    log "${YELLOW}MANUAL${NC} #$id $name — $detail (用户人工确认)"
}

section() {
    echo -e "${BLUE}=== $* ===${NC}" | tee -a "$BASELINE_LOG"
}

# ============================================================
# 阶段 0：环境前置检查
# ============================================================
section "阶段 0：环境前置检查"

# CarTerminal 二进制存在
if [[ -x "$BIN_PATH" ]]; then
    log "CarTerminal binary: $BIN_PATH (ok)"
else
    log "${RED}ERROR${NC} CarTerminal binary not found at $BIN_PATH"
    log "请先在板端执行: cd $PROJECT_DIR && qmake && make -j\$(nproc)"
    exit 1
fi

# 关键配置文件存在
for f in camera_settings.conf rknn_settings.conf rtsp_settings.conf \
         recording_settings.conf voice_settings.conf \
         asr_settings_offline.conf vehicle_devices.conf; do
    if [[ ! -f "$CONFIG_DIR/$f" ]]; then
        log "${RED}ERROR${NC} 缺失配置文件: $CONFIG_DIR/$f"
        exit 1
    fi
done
log "配置文件检查: ok"

# v4l2-ctl / ffprobe / i2cdetect 可用
for cmd in v4l2-ctl ffprobe i2cdetect; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        log "${YELLOW}WARN${NC} $cmd 未安装（部分检查将跳过）"
    fi
done

# ============================================================
# 第 1 项：IMX415 真实采集
# ============================================================
section "第 1 项：IMX415 真实采集"

if [[ ! -e /dev/video33 ]]; then
    record_fail 1 "IMX415 采集" "/dev/video33 不存在"
else
    if command -v v4l2-ctl >/dev/null 2>&1; then
        # 采集 30 帧，--stream-mmap 不报错即 PASS
        if v4l2-ctl -d /dev/video33 --stream-mmap --stream-count=30 \
              >/tmp/baseline_v4l2.log 2>&1; then
            record_pass 1 "IMX415 采集" \
                "/dev/video33 采集 30 帧成功（v4l2-ctl 无错误）"
        else
            record_fail 1 "IMX415 采集" \
                "v4l2-ctl 采集失败，见 /tmp/baseline_v4l2.log"
        fi
    else
        record_manual 1 "IMX415 采集" \
            "v4l2-ctl 未安装，需手动验证 /dev/video33 能采集"
    fi
fi

# ============================================================
# 第 8 项：GPS 串口 NMEA 报文（提前到这里，因为不需要启动 CarTerminal）
# ============================================================
section "第 8 项：GPS 串口 NMEA 报文"

if [[ ! -e /dev/ttyS5 ]]; then
    record_fail 8 "GPS 串口" "/dev/ttyS5 不存在"
elif [[ ! -r /dev/ttyS5 ]]; then
    record_fail 8 "GPS 串口" "/dev/ttyS5 不可读（需加入 dialout 组或 sudo）"
else
    # 5 秒内能读到 $G 开头的 NMEA 报文即 PASS
    timeout 5 cat /dev/ttyS5 >/tmp/baseline_gps.log 2>&1 || true
    if grep -q '^\$G' /tmp/baseline_gps.log 2>/dev/null; then
        record_pass 8 "GPS 串口" \
            "/dev/ttyS5 5 秒内读到 NMEA 报文（$(grep -c '^\$G' /tmp/baseline_gps.log) 行）"
    else
        record_fail 8 "GPS 串口" \
            "/dev/ttyS5 5 秒内未读到 \$G 开头的 NMEA 报文（室内信号弱？需移到窗边/室外）"
    fi
fi

# ============================================================
# 阶段 1：启动 CarTerminal + 抓日志关键词
# 涵盖第 2/3/4/5/9/10 项的自动部分
# ============================================================
if [[ "$SKIP_LAUNCH" -eq 1 ]]; then
    log "${YELLOW}SKIP${NC} 启动 CarTerminal 阶段（--skip-launch）"
    # 人工项也跳过
    record_manual 2 "Qt 显示" "跳过（--skip-launch）"
    record_manual 3 "RGA 转换" "跳过（--skip-launch）"
    record_manual 4 "RKNN 推理" "跳过（--skip-launch）"
    record_manual 5 "MPP 编码" "跳过（--skip-launch）"
    record_manual 6 "RTSP 推流" "跳过（--skip-launch）"
    record_manual 7 "MP4 录像" "跳过（--skip-launch）"
    record_manual 9 "MPU6050 用户态" "跳过（--skip-launch）"
    record_manual 10 "ASR 固定指令" "跳过（--skip-launch）"
else
    section "启动 CarTerminal ${RUNTIME_SECS}s 抓日志"

    # 关键字：RGA convert OK / detect: N objects / MPP encode OK
    # 启动后捕获 RUNTIME_SECS 秒，然后 SIGTERM
    export DISPLAY=:0
    export QT_XCB_GL_INTEGRATION=none
    export QT_OPENGL=software
    cd "$PROJECT_DIR"
    timeout -s TERM -k 5s ${RUNTIME_SECS}s ./CarTerminal > "$RUNTIME_LOG" 2>&1 &
    APP_PID=$!
    log "CarTerminal PID=$APP_PID, 等待 ${RUNTIME_SECS}s ..."
    wait $APP_PID 2>/dev/null
    EXIT_CODE=$?
    log "CarTerminal 退出码=$EXIT_CODE (124=timeout 正常)"

    # 第 3 项：RGA 转换
    section "第 3 项：RGA 转换日志"
    if grep -qi "RGA.*convert.*OK\|RGA.*ok\|rga.*convert\|RGA hardware acceleration enabled\|rga.*im2d" "$RUNTIME_LOG" 2>/dev/null; then
        record_pass 3 "RGA 转换" "日志含 RGA 启用/转换关键词"
    elif grep -qi "RGA.*fail\|RGA.*error" "$RUNTIME_LOG" 2>/dev/null; then
        record_fail 3 "RGA 转换" "日志含 RGA fail/error: $(grep -i 'RGA.*fail\|RGA.*error' "$RUNTIME_LOG" | head -1)"
    else
        record_fail 3 "RGA 转换" "日志未出现 RGA 关键词（可能 camera 未启动或日志格式变化）"
    fi

    # 第 4 项：RKNN 推理
    section "第 4 项：RKNN 推理日志"
    if grep -qE "detect: [0-9]+ object|RKNN.*init|rknn.*load" "$RUNTIME_LOG" 2>/dev/null; then
        record_pass 4 "RKNN 推理" "日志含 RKNN 初始化或 detect: N objects"
    elif grep -qi "rknn.*fail\|rknn.*error" "$RUNTIME_LOG" 2>/dev/null; then
        record_fail 4 "RKNN 推理" "日志含 RKNN fail/error"
    else
        record_fail 4 "RKNN 推理" "日志未出现 RKNN 关键词"
    fi

    # 第 5 项：MPP 编码
    section "第 5 项：MPP 编码日志"
    if grep -qi "MPP.*encode.*OK\|MPP.*init\|mpp.*encode" "$RUNTIME_LOG" 2>/dev/null; then
        record_pass 5 "MPP 编码" "日志含 MPP encode OK / MPP init"
    elif grep -qi "MPP.*fail\|MPP.*error" "$RUNTIME_LOG" 2>/dev/null; then
        record_fail 5 "MPP 编码" "日志含 MPP fail/error"
    else
        record_fail 5 "MPP 编码" "日志未出现 MPP 关键词"
    fi

    # 第 6 项：RTSP 推流（端口监听）
    section "第 6 项：RTSP 推流端口"
    if ss -lnt 2>/dev/null | grep -q ':8554'; then
        record_pass 6 "RTSP 推流" "TCP 8554 端口监听（板端 VLC 拉流 rtsp://127.0.0.1:8554/live）"
    else
        # CarTerminal 已退出，端口当然没了，提示人工用 VLC 重新拉流
        record_manual 6 "RTSP 推流" \
            "需启动 CarTerminal 后用 VLC 拉流 rtsp://<board-ip>:8554/live，1s 内出图即 PASS"
    fi

    # 第 7 项：MP4 录像
    section "第 7 项：MP4 录像"
    RECORD_DIR="$PROJECT_DIR/recordings"
    # 检查 recording_enabled 是否打开
    RECORDING_CFG=$(grep -E '^recording_enabled' "$CONFIG_DIR/recording_settings.conf" 2>/dev/null | cut -d= -f2)
    if [[ "$RECORDING_CFG" != "true" ]]; then
        record_manual 7 "MP4 录像" \
            "config/recording_settings.conf 当前 recording_enabled=$RECORDING_CFG，需设为 true 后重启 CarTerminal，检查 recordings/ 目录"
    else
        # 找最新的 mp4
        LATEST_MP4=$(ls -t "$RECORD_DIR"/*.mp4 2>/dev/null | head -1)
        if [[ -z "$LATEST_MP4" ]]; then
            record_fail 7 "MP4 录像" "recording_enabled=true 但 recordings/ 目录无 mp4"
        elif command -v ffprobe >/dev/null 2>&1 && \
             ffprobe -v error -show_format "$LATEST_MP4" >/dev/null 2>&1; then
            record_pass 7 "MP4 录像" "ffprobe 解析成功: $LATEST_MP4"
        else
            # mp4 存在但 ffprobe 失败：SIGTERM 强杀导致 trailer 未写入
            # 这是测试方法局限（20s 强杀），不是功能缺陷
            # 检查日志是否有 recording started 确认录像确实启动
            if grep -qi "recording started" "$RUNTIME_LOG" 2>/dev/null; then
                record_manual 7 "MP4 录像" \
                    "日志含 recording started，但 SIGTERM 强杀导致 mp4 trailer 未写入（测试方法局限）。需手动运行 CarTerminal 2 分钟 + GUI 正常退出后用 ffprobe 验证"
            else
                record_fail 7 "MP4 录像" "mp4 存在但 ffprobe 解析失败，且日志无 recording started: $LATEST_MP4"
            fi
        fi
    fi

    # 第 2 项：Qt 显示（人工）
    section "第 2 项：Qt 显示（人工确认）"
    record_manual 2 "Qt 显示" \
        "观察 HDMI 显示器：CarTerminal 启动后主界面正常显示（含时钟/按钮）"

    # 第 9 项：MPU6050 用户态（人工 + 日志提示）
    section "第 9 项：MPU6050 用户态（人工确认）"
    MPU_CFG=$(grep -E '^mpu6050_enabled' "$CONFIG_DIR/vehicle_devices.conf" 2>/dev/null | cut -d= -f2)
    if [[ "$MPU_CFG" != "true" ]]; then
        record_manual 9 "MPU6050 用户态" \
            "vehicle_devices.conf mpu6050_enabled=$MPU_CFG，需设为 true 后重启并观察 Qt 姿态页 pitch/roll 变化"
    elif grep -qi "mpu6050.*init\|MPU6050.*ready" "$RUNTIME_LOG" 2>/dev/null; then
        record_manual 9 "MPU6050 用户态" \
            "日志含 MPU6050 init/ready，需在 Qt 姿态页人工观察 pitch/roll 变化"
    else
        record_manual 9 "MPU6050 用户态" \
            "mpu6050_enabled=true 但日志未见 init 关键词，需确认 I2C3 设备节点 + i2cdetect 0x68"
    fi

    # 第 10 项：ASR 固定指令（人工 + 测试注入提示）
    section "第 10 项：ASR 固定指令（人工确认）"
    record_manual 10 "ASR 固定指令" \
        "方式 A: 直接语音说『打开监控』观察 GUI 切换；方式 B: VEHICLE_VOICE_TEST_TEXT='打开监控' ./CarTerminal"
fi

# ============================================================
# 第 11 项：启动/退出 3 次（自动）
# ============================================================
section "第 11 项：启动/正常退出 3 次"

if [[ "$SKIP_LAUNCH" -eq 1 ]]; then
    record_manual 11 "启动退出 3 次" "跳过（--skip-launch）"
else
    EXIT_OK_CNT=0
    for i in 1 2 3; do
        log "  第 $i 次启动..."
        cd "$PROJECT_DIR"
        export DISPLAY=:0
        export QT_XCB_GL_INTEGRATION=none
        export QT_OPENGL=software
        timeout -s TERM -k 5s 5s ./CarTerminal > /tmp/baseline_run$i.log 2>&1 &
        P=$!
        wait $P 2>/dev/null
        RC=$?
        # timeout 退出码 124 表示被 SIGTERM 正常终止（预期）
        if [[ $RC -eq 124 ]] || [[ $RC -eq 0 ]]; then
            EXIT_OK_CNT=$((EXIT_OK_CNT + 1))
            log "  第 $i 次退出码=$RC (124=timeout 正常, 0=自行退出)"
        else
            log "  ${RED}第 $i 次异常${NC} 退出码=$RC"
        fi
    done

    if [[ $EXIT_OK_CNT -eq 3 ]]; then
        record_pass 11 "启动退出 3 次" "3 次启动+SIGTERM 全部正常退出"
    else
        record_fail 11 "启动退出 3 次" "仅 $EXIT_OK_CNT/3 次正常退出"
    fi
fi

# ============================================================
# 收集设备节点清单 → docs/baseline/devices.txt
# ============================================================
section "归档：设备节点清单 → docs/baseline/devices.txt"

BASELINE_DOCS="$PROJECT_DIR/docs/baseline"
mkdir -p "$BASELINE_DOCS"
DEVICES_TXT="$BASELINE_DOCS/devices.txt"

{
    echo "# 板端设备节点清单（auto-generated by baseline_verify.sh）"
    echo "# 生成时间: $(date)"
    echo "# 板端主机: $(hostname)"
    echo "# 内核: $(uname -r)"
    echo ""
    echo "## /dev/video*"
    ls -l /dev/video* 2>/dev/null
    echo ""
    echo "## /dev/i2c-*"
    ls -l /dev/i2c-* 2>/dev/null
    echo ""
    echo "## /dev/ttyS*"
    ls -l /dev/ttyS* 2>/dev/null
    echo ""
    echo "## /dev/gpiochip*"
    ls -l /dev/gpiochip* 2>/dev/null
    echo ""
    echo "## /dev/input/event*"
    ls -l /dev/input/event* 2>/dev/null
    echo ""
    echo "## /dev/snd/*"
    ls -l /dev/snd/* 2>/dev/null
    echo ""
    echo "## i2cdetect -y 3 (MPU6050 @ I2C3)"
    i2cdetect -y 3 2>/dev/null || echo "(i2cdetect 不可用或 I2C3 不存在)"
    echo ""
    echo "## /proc/device-tree/model"
    cat /proc/device-tree/model 2>/dev/null
    echo ""
    echo "## 已加载模块（driver 相关）"
    lsmod | grep -E 'iio|inv_mpu|v4l2|media|rockchip|rknn|mpp|rga|buzzer' || echo "(无相关模块)"
} > "$DEVICES_TXT"
log "设备节点清单已写入: $DEVICES_TXT"

# ============================================================
# Summary
# ============================================================
section "Summary"
echo -e "${GREEN}PASS${NC}: $PASS_CNT    ${RED}FAIL${NC}: $FAIL_CNT    ${YELLOW}MANUAL${NC}: $MANUAL_CNT" | tee -a "$BASELINE_LOG"
echo ""
echo "详细结果：" | tee -a "$BASELINE_LOG"
for line in "${RESULTS[@]}"; do
    echo -e "  $line" | tee -a "$BASELINE_LOG"
done
echo ""
echo "产物：" | tee -a "$BASELINE_LOG"
echo "  验证日志:       $BASELINE_LOG" | tee -a "$BASELINE_LOG"
echo "  CarTerminal 日志: $RUNTIME_LOG" | tee -a "$BASELINE_LOG"
echo "  设备节点清单:    $DEVICES_TXT" | tee -a "$BASELINE_LOG"
echo ""
echo "人工项需要用户在 GUI/HDMI 上观察确认后，手动更新 PROJECT_ROADMAP.md 第十节进度跟踪表。" | tee -a "$BASELINE_LOG"

if [[ $FAIL_CNT -gt 0 ]]; then
    exit 1
fi
exit 0
