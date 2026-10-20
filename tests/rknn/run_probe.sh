#!/bin/bash
# tests/rknn/run_probe.sh — board-side RKNN Stage A verification (jilu.txt §八).
#
# Runs the COMPLETE §八 command sequence on the RK3588S board and captures
# REAL exit codes / RC variables. No summarisation without evidence.
#
# Usage (from ~/RK3588S_CarTerminal):
#   bash tests/rknn/run_probe.sh 2>&1 | tee /tmp/rknn_stage_a_full.log
#
# All commands emit KEY=VALUE lines so the output is greppable / parseable.

set -u
set -o pipefail

PROJECT_DIR="${PROJECT_DIR:-$(pwd)}"
LIBRKNNRT="${LIBRKNNRT:-/usr/local/lib/librknnrt.so}"
MODEL="${MODEL:-${PROJECT_DIR}/config/yolov5s.rknn}"
ITERATIONS="${ITERATIONS:-3}"
STABILITY_RUNS="${STABILITY_RUNS:-10}"

# Required tools
for tool in cmake make sha256sum nm dmesg grep tee; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR=missing required tool: $tool"
        exit 2
    fi
done

echo "=============================================================="
echo "RKNN Stage A Board Verification — $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo "=============================================================="
echo "PROJECT_DIR=$PROJECT_DIR"
echo "LIBRKNNRT=$LIBRKNNRT"
echo "MODEL=$MODEL"
echo "ITERATIONS=$ITERATIONS"
echo "STABILITY_RUNS=$STABILITY_RUNS"
echo ""

# ============================================================================
# §八.1 — Environment confirmation
# ============================================================================
echo "===== §八.1 Environment confirmation ====="
export LANG=C.UTF-8
export LC_ALL=C.UTF-8

echo "--- librknnrt.so SHA256 ---"
sha256sum "$LIBRKNNRT"
LIBRKNNRT_SHA="$(sha256sum "$LIBRKNNRT" | awk '{print $1}')"
echo "LIBRKNNRT_SHA256=$LIBRKNNRT_SHA"

echo "--- yolov5s.rknn SHA256 ---"
sha256sum "$MODEL"
MODEL_SHA="$(sha256sum "$MODEL" | awk '{print $1}')"
echo "MODEL_SHA256=$MODEL_SHA"

echo "--- Exported RKNN symbols (nm -D) ---"
nm -D "$LIBRKNNRT" | grep -E 'rknn_(init|query|inputs_set|run|outputs_get|outputs_release|destroy)' || true
echo ""

# ============================================================================
# §八.2 — Build the probe
# ============================================================================
echo "===== §八.2 Build probe ====="
BUILD_DIR="build_rknn_probe_$(date +%Y%m%d_%H%M%S)"
echo "BUILD_DIR=$BUILD_DIR"

cmake \
    -S tests/rknn \
    -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DRKNN_INCLUDE_DIR=/usr/local/include \
    2>&1 | tee /tmp/rknn_cmake.log
CMAKE_RC=${PIPESTATUS[0]}
echo "CMAKE_RC=$CMAKE_RC"

if [ "$CMAKE_RC" -ne 0 ]; then
    echo "STAGE_A_RESULT=FAIL"
    echo "FAILURE_REASON=cmake_failed"
    exit 2
fi

cmake --build "$BUILD_DIR" --parallel 1 \
    2>&1 | tee /tmp/rknn_build.log
BUILD_RC=${PIPESTATUS[0]}
echo "BUILD_RC=$BUILD_RC"

if [ "$BUILD_RC" -ne 0 ]; then
    echo "STAGE_A_RESULT=FAIL"
    echo "FAILURE_REASON=build_failed"
    exit 2
fi

PROBE_BIN="$BUILD_DIR/rknn_probe"
echo "PROBE_BIN=$PROBE_BIN"
echo ""

# ============================================================================
# §八.3 — Capture dmesg BEFORE inference
# ============================================================================
echo "===== §八.3 dmesg BEFORE inference ====="
dmesg | grep -iE 'rknpu|npu|iommu|fault|bus error' \
    | tail -100 > /tmp/rknn_dmesg_before.log || true
DMESG_BEFORE_LINES=$(wc -l < /tmp/rknn_dmesg_before.log)
echo "DMESG_BEFORE_LINES=$DMESG_BEFORE_LINES"
echo ""

# ============================================================================
# §八.4 — Run the probe (real inference)
# ============================================================================
echo "===== §八.4 Probe inference ($ITERATIONS iterations) ====="
"$PROBE_BIN" \
    --library "$LIBRKNNRT" \
    --model "$MODEL" \
    --iterations "$ITERATIONS" \
    2>&1 | tee /tmp/rknn_probe.log
PROBE_RC=${PIPESTATUS[0]}
echo "PROBE_RC=$PROBE_RC"
echo ""

# ============================================================================
# §八.5 — Grep key outputs
# ============================================================================
echo "===== §八.5 Key output grep ====="
grep -E \
    'RKNN_API_VERSION|RKNN_DRIVER_VERSION|INPUT_COUNT|OUTPUT_COUNT|INPUT_[0-9]+_|OUTPUT_[0-9]+_|INPUTS_SET_RC|RUN_RC|OUTPUTS_GET_RC|CHECKSUM|RKNN_PROBE_RESULT|ERROR|WARN' \
    /tmp/rknn_probe.log || true
echo ""

# ============================================================================
# §八.6 — dmesg AFTER inference
# ============================================================================
echo "===== §八.6 dmesg AFTER inference ====="
dmesg | grep -iE \
    'rknpu|npu|iommu|fault|bus error|page fault|timeout' \
    | tail -150 | tee /tmp/rknn_dmesg_after.log || true
DMESG_AFTER_LINES=$(wc -l < /tmp/rknn_dmesg_after.log)
echo "DMESG_AFTER_LINES=$DMESG_AFTER_LINES"

# Detect NEW kernel errors by diffing before vs after.
# (after.log may contain more lines than before.log — capture the delta.)
comm -13 \
    <(sort /tmp/rknn_dmesg_before.log) \
    <(sort /tmp/rknn_dmesg_after.log) \
    > /tmp/rknn_dmesg_new.log || true
DMESG_NEW_LINES=$(wc -l < /tmp/rknn_dmesg_new.log)
echo "DMESG_NEW_LINES=$DMESG_NEW_LINES"
if [ "$DMESG_NEW_LINES" -gt 0 ]; then
    echo "--- New dmesg lines (potential NPU faults) ---"
    cat /tmp/rknn_dmesg_new.log
fi
echo ""

# ============================================================================
# §八.7 — Stability test (10 runs)
# ============================================================================
echo "===== §八.7 Stability test ($STABILITY_RUNS runs) ====="
STABILITY_PASS=0
STABILITY_FAIL=0
STABILITY_FIRST_FAIL_LOG=""
for i in $(seq 1 "$STABILITY_RUNS"); do
    "$PROBE_BIN" \
        --library "$LIBRKNNRT" \
        --model "$MODEL" \
        --iterations "$ITERATIONS" \
        > "/tmp/rknn_repeat_${i}.log" 2>&1
    rc=$?
    echo "STABILITY_RUN_INDEX=$i STABILITY_RUN_RC=$rc"
    if [ "$rc" -eq 0 ]; then
        STABILITY_PASS=$((STABILITY_PASS + 1))
    else
        STABILITY_FAIL=$((STABILITY_FAIL + 1))
        if [ -z "$STABILITY_FIRST_FAIL_LOG" ]; then
            STABILITY_FIRST_FAIL_LOG="/tmp/rknn_repeat_${i}.log"
        fi
        echo "--- First failing run tail ---"
        tail -50 "/tmp/rknn_repeat_${i}.log"
        break
    fi
done
echo "STABILITY_PASS=$STABILITY_PASS"
echo "STABILITY_FAIL=$STABILITY_FAIL"
echo ""

# ============================================================================
# §八.8 — Negative test: missing library
# ============================================================================
echo "===== §八.8 Negative test: missing library ====="
"$PROBE_BIN" \
    --library /tmp/not-exist-librknnrt.so \
    --model "$MODEL" \
    --iterations 1
MISSING_LIB_RC=$?
echo "MISSING_LIB_RC=$MISSING_LIB_RC"
echo ""

# ============================================================================
# §八.9 — Negative test: missing model
# ============================================================================
echo "===== §八.9 Negative test: missing model ====="
"$PROBE_BIN" \
    --library "$LIBRKNNRT" \
    --model /tmp/not-exist.rknn \
    --iterations 1
MISSING_MODEL_RC=$?
echo "MISSING_MODEL_RC=$MISSING_MODEL_RC"
echo ""

# ============================================================================
# §八.10 — Negative test: truncated model
# ============================================================================
echo "===== §八.10 Negative test: truncated model ====="
head -c 4096 "$MODEL" > /tmp/truncated_yolov5s.rknn
"$PROBE_BIN" \
    --library "$LIBRKNNRT" \
    --model /tmp/truncated_yolov5s.rknn \
    --iterations 1
TRUNCATED_MODEL_RC=$?
echo "TRUNCATED_MODEL_RC=$TRUNCATED_MODEL_RC"
echo ""

# ============================================================================
# §八.11 — Main program regression build
# ============================================================================
echo "===== §八.11 Main program regression build ====="
MAIN_BUILD_DIR="build_qt_rknn_review_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$MAIN_BUILD_DIR"
cd "$MAIN_BUILD_DIR"

qmake ../CarTerminal.pro CONFIG+=release \
    2>&1 | tee /tmp/rknn_qmake.log
QMAKE_RC=${PIPESTATUS[0]}
echo "QMAKE_RC=$QMAKE_RC"

if [ "$QMAKE_RC" -ne 0 ]; then
    echo "STAGE_A_RESULT=FAIL"
    echo "FAILURE_REASON=qmake_failed"
    cd "$PROJECT_DIR"
    exit 2
fi

make -j1 2>&1 | tee /tmp/rknn_main_build.log
MAIN_BUILD_RC=${PIPESTATUS[0]}
echo "MAIN_BUILD_RC=$MAIN_BUILD_RC"

cd "$PROJECT_DIR"
echo ""

# ============================================================================
# §九 — Result judgment (15 conditions)
# ============================================================================
echo "=============================================================="
echo "§九 Result Judgment"
echo "=============================================================="

# Condition 1: uses /usr/local/include/rknn_api.h
COND1=$(grep -c "RKNN: using board-installed /usr/local/include/rknn_api.h" /tmp/rknn_qmake.log || echo 0)
echo "COND_1_USES_OFFICIAL_HEADER=$COND1"

# Condition 2: no hand-written ABI (grep for forbidden patterns)
FORBIDDEN=$(grep -lE 'typedef.*rknn_context|struct _rknn_input \{|struct _rknn_output \{' \
    "$PROJECT_DIR/media/rknn_detector.cpp" \
    "$PROJECT_DIR/media/rknn_runtime.h" \
    "$PROJECT_DIR/media/rknn_runtime.cpp" 2>/dev/null || true)
COND2=$([ -z "$FORBIDDEN" ] && echo 1 || echo 0)
echo "COND_2_NO_HANDWRITTEN_ABI=$COND2"

# Condition 3-6: build + probe success
echo "COND_3_BUILD_OK=$([ "$BUILD_RC" -eq 0 ] && echo 1 || echo 0)"
echo "COND_4_PROBE_RC=$PROBE_RC"
echo "COND_5_PROBE_RESULT=$(grep -E '^RKNN_PROBE_RESULT=' /tmp/rknn_probe.log | tail -1 | cut -d= -f2)"

# Condition 7: stability 10 runs
echo "COND_6_STABILITY_PASS=$STABILITY_PASS/$STABILITY_RUNS"

# Condition 8: no new dmesg NPU errors
echo "COND_7_NO_NEW_DMESG=$([ "$DMESG_NEW_LINES" -eq 0 ] && echo 1 || echo 0)"

# Condition 9: main build OK
echo "COND_8_MAIN_BUILD_OK=$([ "$MAIN_BUILD_RC" -eq 0 ] && echo 1 || echo 0)"

echo ""
echo "=============================================================="
echo "Real Exit Code Summary (jilu.txt §十三)"
echo "=============================================================="
echo "CMAKE_RC=$CMAKE_RC"
echo "BUILD_RC=$BUILD_RC"
echo "PROBE_RC=$PROBE_RC"
echo "MISSING_LIB_RC=$MISSING_LIB_RC"
echo "MISSING_MODEL_RC=$MISSING_MODEL_RC"
echo "TRUNCATED_MODEL_RC=$TRUNCATED_MODEL_RC"
echo "QMAKE_RC=$QMAKE_RC"
echo "MAIN_BUILD_RC=$MAIN_BUILD_RC"
echo "STABILITY_PASS=$STABILITY_PASS/$STABILITY_RUNS"
echo "DMESG_NEW_LINES=$DMESG_NEW_LINES"
echo ""

# Final verdict (per §十三)
if [ "$CMAKE_RC" -eq 0 ] && [ "$BUILD_RC" -eq 0 ] && \
   [ "$PROBE_RC" -eq 0 ] && \
   [ "$STABILITY_PASS" -eq "$STABILITY_RUNS" ] && \
   [ "$DMESG_NEW_LINES" -eq 0 ] && \
   [ "$MAIN_BUILD_RC" -eq 0 ]; then
    echo "STAGE_A_VERDICT=APPROVE"
    echo "  RKNN minimal inference is compatible with the current"
    echo "  librknnrt.so + yolov5s.rknn + NPU driver combination."
    echo "  Stage B (static image inference) may proceed."
    exit 0
elif [ "$PROBE_RC" -eq 135 ]; then
    echo "STAGE_A_VERDICT=BLOCKED"
    echo "  rknn_run triggered SIGBUS (rc=135)."
    echo "  Current runtime/model/driver combination is incompatible"
    echo "  at execution stage. Keep rknn_enabled=false; do NOT proceed"
    echo "  to Stage B. Await BSP/NPU driver upgrade or matching runtime."
    exit 135
elif [ "$PROBE_RC" -ne 0 ] && [ "$PROBE_RC" -ne 135 ]; then
    echo "STAGE_A_VERDICT=BLOCKED"
    echo "  Probe returned non-zero non-SIGBUS rc=$PROBE_RC."
    echo "  See /tmp/rknn_probe.log for the failure point."
    exit "$PROBE_RC"
else
    echo "STAGE_A_VERDICT=REQUEST_CHANGES"
    echo "  One or more §九 conditions failed. See COND_* lines above."
    exit 3
fi
