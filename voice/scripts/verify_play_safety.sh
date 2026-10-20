#!/usr/bin/env bash
# =============================================================================
# 板端固定语音指令安全闭环验证脚本
# 对应 jilu.txt 第七至十六节。只执行真实命令，捕获真实 RC，不伪造任何结果。
#
# 用法（板端执行）：
#   cd <PROJECT_DIR>
#   bash voice/scripts/verify_play_safety.sh
#
# 可选环境变量：
#   PROJECT_DIR  项目根目录（默认 <PROJECT_DIR>）
#   KEEP_BUILD=1 复用已有构建目录（默认行为：优先复用 build_voice_hotword_verify_*）
# =============================================================================
set -u

PROJECT_DIR="${PROJECT_DIR:-$HOME/RK3588S_CarTerminal}"
cd "$PROJECT_DIR" || { echo "FATAL: 项目目录不存在 $PROJECT_DIR"; exit 2; }

FAIL=0
PASSED=0

phase() { echo; echo "========== $1 =========="; }
record() {
  # record <name> <rc> [expected_rc]
  local name="$1" rc="$2" expect="${3:-0}"
  if [ "$rc" -eq "$expect" ]; then
    echo "[PASS] $name (rc=$rc)"
    PASSED=$((PASSED+1))
  else
    echo "[FAIL] $name (rc=$rc, expected=$expect)"
    FAIL=$((FAIL+1))
  fi
}

# ---------------------------------------------------------------------------
# 5.2 查找/创建构建目录
# ---------------------------------------------------------------------------
phase "5.2 查找/创建构建目录"
REVIEW_BUILD=$(ls -dt ./build_voice_hotword_verify_* 2>/dev/null | head -1)
if [ -z "$REVIEW_BUILD" ]; then
  REVIEW_BUILD="./build_voice_play_safety_$(date +%Y%m%d_%H%M%S)"
  echo "创建新构建目录: $REVIEW_BUILD"
  cmake -S voice -B "$REVIEW_BUILD" -DCMAKE_BUILD_TYPE=Release
  record "cmake configure" $?
else
  echo "复用已有构建目录: $REVIEW_BUILD"
fi
echo "REVIEW_BUILD=$REVIEW_BUILD"

# ---------------------------------------------------------------------------
# 7 GREEN：构建并运行 test_vehiclecommand
# ---------------------------------------------------------------------------
phase "7 构建并运行 test_vehiclecommand"
cmake --build "$REVIEW_BUILD" --target test_vehiclecommand --parallel 1
record "build test_vehiclecommand" $?

"$REVIEW_BUILD/test_vehiclecommand" 2>&1 | tee /tmp/vehiclecommand_green.log
GREEN_RC=${PIPESTATUS[0]}
echo "GREEN_RC=$GREEN_RC"
record "run test_vehiclecommand (GREEN)" "$GREEN_RC"

# ---------------------------------------------------------------------------
# 8 完整 CTest 回归
# ---------------------------------------------------------------------------
phase "8 完整 CTest 回归"
(
  cd "$REVIEW_BUILD" || exit 2
  ctest --output-on-failure
)
CTEST_RC=$?
echo "CTEST_RC=$CTEST_RC"
record "ctest" "$CTEST_RC"

# ---------------------------------------------------------------------------
# 9 重新验证原始失败样本（播放列表）
# ---------------------------------------------------------------------------
phase "9 重新验证原始失败样本 播放列表"
cmake --build "$REVIEW_BUILD" --target asr_wav_test tts_wav_test --parallel 1
record "build asr_wav_test tts_wav_test" $?

if [ -f /tmp/negative_6.wav ]; then
  echo "复用 /tmp/negative_6.wav"
  PLAY_LIST_WAV=/tmp/negative_6.wav
else
  echo "/tmp/negative_6.wav 不存在，重新生成"
  "$REVIEW_BUILD/tts_wav_test" \
    --model ./config/single_speaker_fast.bin \
    --text "播放列表" \
    --output /tmp/play_list_after_fix.wav
  record "tts 播放列表" $?
  PLAY_LIST_WAV=/tmp/play_list_after_fix.wav
fi

"$REVIEW_BUILD/asr_wav_test" \
  --config ./config/asr_settings_offline.conf \
  --wav "$PLAY_LIST_WAV" \
  --expect-command Unknown \
  2>&1 | tee /tmp/play_list_after_fix.log
PLAY_LIST_RC=${PIPESTATUS[0]}
echo "PLAY_LIST_RC=$PLAY_LIST_RC"

echo "--- 关键日志 ---"
grep -E 'ASR_HOTWORDS_SCORE|ASR_TEXT|COMMAND|EXPECTED_COMMAND|ASR_RESULT|ERROR' \
  /tmp/play_list_after_fix.log || echo "(未匹配到关键日志)"
record "asr 播放列表 -> Unknown" "$PLAY_LIST_RC"

# ---------------------------------------------------------------------------
# 10 正向 accuracy 验证
# ---------------------------------------------------------------------------
phase "10 正向 accuracy 验证"
bash voice/scripts/verify_tts_asr_wav.sh \
  --profile accuracy \
  --build-dir "$REVIEW_BUILD" \
  --model ./config/single_speaker_fast.bin \
  --config ./config/asr_settings_offline.conf \
  --keep-success \
  2>&1 | tee /tmp/accuracy_after_play_fix.log
ACCURACY_RC=${PIPESTATUS[0]}
echo "ACCURACY_RC=$ACCURACY_RC"
record "accuracy" "$ACCURACY_RC"

# ---------------------------------------------------------------------------
# 11 smoke 验证
# ---------------------------------------------------------------------------
phase "11 smoke 验证"
bash voice/scripts/verify_tts_asr_wav.sh \
  --profile smoke \
  --build-dir "$REVIEW_BUILD" \
  --model ./config/single_speaker_fast.bin \
  --config ./config/asr_settings_offline.conf \
  --keep-success \
  2>&1 | tee /tmp/smoke_after_play_fix.log
SMOKE_RC=${PIPESTATUS[0]}
echo "SMOKE_RC=$SMOKE_RC"
record "smoke" "$SMOKE_RC"

# ---------------------------------------------------------------------------
# 12 完整负向测试
# ---------------------------------------------------------------------------
phase "12 完整负向测试"
NEGATIVE_FAILED=0
INDEX=0
for TEXT in \
    "讲个笑话" \
    "今天星期几" \
    "介绍一下自己" \
    "帮我写代码" \
    "我要吃饭" \
    "播放列表"
do
  INDEX=$((INDEX + 1))
  WAV="/tmp/negative_after_${INDEX}.wav"
  LOG="/tmp/negative_after_${INDEX}.log"
  echo "----- NEGATIVE [$INDEX]: $TEXT -----"

  "$REVIEW_BUILD/tts_wav_test" \
    --model ./config/single_speaker_fast.bin \
    --text "$TEXT" \
    --output "$WAV"
  TTS_RC=$?
  if [ "$TTS_RC" -ne 0 ]; then
    echo "[FAIL] TTS 失败: $TEXT (tts_rc=$TTS_RC)"
    NEGATIVE_FAILED=$((NEGATIVE_FAILED + 1))
    continue
  fi

  "$REVIEW_BUILD/asr_wav_test" \
    --config ./config/asr_settings_offline.conf \
    --wav "$WAV" \
    --expect-command Unknown \
    2>&1 | tee "$LOG"
  ASR_RC=${PIPESTATUS[0]}

  grep -E 'ASR_TEXT|COMMAND|EXPECTED_COMMAND|ASR_RESULT|ERROR' "$LOG" || true

  if [ "$ASR_RC" -ne 0 ]; then
    echo "[FAIL] 误触发指令: $TEXT (asr_rc=$ASR_RC)"
    NEGATIVE_FAILED=$((NEGATIVE_FAILED + 1))
  else
    echo "[PASS] 保持 Unknown: $TEXT"
  fi
done
echo "NEGATIVE_FAILED=$NEGATIVE_FAILED"
record "negative suite (期望 NEGATIVE_FAILED=0)" "$NEGATIVE_FAILED"

# ---------------------------------------------------------------------------
# 13 配置保护检查
# ---------------------------------------------------------------------------
phase "13 配置保护检查"
echo "--- asr_settings_offline.conf ---"
grep -nE 'asr_decoding_method|asr_hotwords_file|asr_hotwords_score' \
  config/asr_settings_offline.conf || echo "(未找到配置项)"

echo "--- 检查禁止热词 ---"
if grep -n "打开茬" config/asr_hotwords.txt 2>/dev/null; then
  echo "[FAIL] 发现禁止热词 打开茬"
  FAIL=$((FAIL+1))
else
  echo "FORBIDDEN_HOTWORD=NONE"
  PASSED=$((PASSED+1))
fi

# ---------------------------------------------------------------------------
# 14 重新编译主程序
# ---------------------------------------------------------------------------
phase "14 重新编译主程序"
ls -l CarTerminal.pro Makefile 2>/dev/null || echo "(CarTerminal.pro/Makefile 状态如上)"
if [ -f Makefile ]; then
  make -j2 2>&1 | tee /tmp/car_terminal_after_play_fix.log
  MAIN_BUILD_RC=${PIPESTATUS[0]}
else
  qmake CarTerminal.pro
  QMAKE_RC=$?
  echo "QMAKE_RC=$QMAKE_RC"
  if [ "$QMAKE_RC" -eq 0 ]; then
    make -j2 2>&1 | tee /tmp/car_terminal_after_play_fix.log
    MAIN_BUILD_RC=${PIPESTATUS[0]}
  else
    MAIN_BUILD_RC=$QMAKE_RC
  fi
fi
echo "MAIN_BUILD_RC=$MAIN_BUILD_RC"
record "main build" "$MAIN_BUILD_RC"

# ---------------------------------------------------------------------------
# 15 GUI 文字注入回归
# ---------------------------------------------------------------------------
phase "15 GUI 文字注入回归"
echo "--- 播放音乐 ---"
timeout 15s env \
  DISPLAY=:0 \
  QT_XCB_GL_INTEGRATION=none \
  QT_OPENGL=software \
  VEHICLE_VOICE_TEST_TEXT="播放音乐" \
  ./CarTerminal \
  2>&1 | tee /tmp/gui_play_music.log
GUI_PLAY_MUSIC_RC=${PIPESTATUS[0]}
echo "GUI_PLAY_MUSIC_RC=$GUI_PLAY_MUSIC_RC"
echo "--- 播放音乐 关键日志 ---"
grep -E 'injecting voice test text|voice command matched|MusicPlay|unmatched' \
  /tmp/gui_play_music.log || echo "(未匹配)"

echo "--- 孤立 播放 ---"
timeout 15s env \
  DISPLAY=:0 \
  QT_XCB_GL_INTEGRATION=none \
  QT_OPENGL=software \
  VEHICLE_VOICE_TEST_TEXT="播放" \
  ./CarTerminal \
  2>&1 | tee /tmp/gui_play_alone.log
GUI_PLAY_ALONE_RC=${PIPESTATUS[0]}
echo "GUI_PLAY_ALONE_RC=$GUI_PLAY_ALONE_RC"
echo "--- 孤立 播放 关键日志 ---"
grep -E 'injecting voice test text|voice command matched|MusicPlay|unmatched' \
  /tmp/gui_play_alone.log || echo "(未匹配)"

# timeout=124 可接受；SIGSEGV(139)/SIGBUS(135)/abort(134) 不可接受
if [ "$GUI_PLAY_ALONE_RC" -eq 124 ] || [ "$GUI_PLAY_ALONE_RC" -eq 0 ]; then
  echo "[PASS] GUI 孤立播放 未崩溃 (rc=$GUI_PLAY_ALONE_RC)"
  PASSED=$((PASSED+1))
elif [ "$GUI_PLAY_ALONE_RC" -ge 128 ]; then
  echo "[FAIL] GUI 孤立播放 信号崩溃 (rc=$GUI_PLAY_ALONE_RC)"
  FAIL=$((FAIL+1))
else
  echo "[INFO] GUI 孤立播放 rc=$GUI_PLAY_ALONE_RC (需人工确认)"
fi

# ---------------------------------------------------------------------------
# 16 代码审查
# ---------------------------------------------------------------------------
phase "16 代码审查"
echo "--- 检查 kZhPlayExact 是否已移除 ---"
if grep -R -n 'kZhPlayExact' common voice/tests tests 2>/dev/null; then
  echo "[FAIL] 仍存在 kZhPlayExact"
  FAIL=$((FAIL+1))
else
  echo "KZH_PLAY_EXACT_REMOVED=1"
  PASSED=$((PASSED+1))
fi

echo "--- 检查其他禁止模式 ---"
grep -R -nE 'value == "播放"|contains\("播放"\)' common 2>/dev/null \
  && { echo "[FAIL] 发现禁止匹配模式"; FAIL=$((FAIL+1)); } \
  || echo "FORBIDDEN_PATTERN=NONE"

echo "--- git status ---"
git status --short 2>/dev/null || echo "(非 git 仓库或 git 不可用)"

# ---------------------------------------------------------------------------
# 最终汇总
# ---------------------------------------------------------------------------
phase "最终汇总"
echo "PASSED=$PASSED"
echo "FAIL=$FAIL"
echo "GREEN_RC=$GREEN_RC"
echo "CTEST_RC=$CTEST_RC"
echo "PLAY_LIST_RC=$PLAY_LIST_RC"
echo "ACCURACY_RC=$ACCURACY_RC"
echo "SMOKE_RC=$SMOKE_RC"
echo "NEGATIVE_FAILED=$NEGATIVE_FAILED"
echo "MAIN_BUILD_RC=$MAIN_BUILD_RC"
echo "GUI_PLAY_MUSIC_RC=$GUI_PLAY_MUSIC_RC"
echo "GUI_PLAY_ALONE_RC=$GUI_PLAY_ALONE_RC"

if [ "$FAIL" -eq 0 ]; then
  echo "VERDICT=ALL_PASS"
  exit 0
else
  echo "VERDICT=HAS_FAILURES"
  exit 1
fi
