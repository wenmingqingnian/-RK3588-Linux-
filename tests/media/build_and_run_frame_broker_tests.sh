#!/bin/bash
# build_and_run_frame_broker_tests.sh
# ----------------------------------------------------------------------------
# Build and run the FrameBroker tests on the board (Linux). Verifies the
# fixed-capacity ring buffer refactor preserves all existing behavior and
# exercises new ring-specific edge cases.
#
# Tests covered:
#   1. test_frame_broker         — original 8-scenario contract tests
#                                  (regression baseline; verifies the
#                                  ring refactor preserved behavior)
#   2. test_frame_broker_ring    — new 8-scenario ring-specific tests
#                                  (capacity-1, wrap-around, move semantics,
#                                   slot release, 1-producer/3-consumer
#                                   stress, enable/disable cycling)
#
# Exit codes:
#   0 — all tests passed
#   1 — build or test failure
# ----------------------------------------------------------------------------
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

echo "==> Project root: ${PROJECT_ROOT}"
mkdir -p "${BUILD_DIR}"

FAIL=0

# ---------------------------------------------------------------------------
# Test 1: original contract tests (regression baseline)
# ---------------------------------------------------------------------------
echo "==> Building test_frame_broker"
g++ -std=c++17 -Wall -Wextra -pthread \
    -I "${PROJECT_ROOT}/media" \
    "${SCRIPT_DIR}/test_frame_broker.cpp" \
    "${PROJECT_ROOT}/media/frame_broker.cpp" \
    -o "${BUILD_DIR}/test_frame_broker" 2>&1

echo "==> Running test_frame_broker"
"${BUILD_DIR}/test_frame_broker" || FAIL=1

# ---------------------------------------------------------------------------
# Test 2: new ring-specific tests
# ---------------------------------------------------------------------------
echo "==> Building test_frame_broker_ring"
g++ -std=c++17 -Wall -Wextra -pthread \
    -I "${PROJECT_ROOT}/media" \
    "${SCRIPT_DIR}/test_frame_broker_ring.cpp" \
    "${PROJECT_ROOT}/media/frame_broker.cpp" \
    -o "${BUILD_DIR}/test_frame_broker_ring" 2>&1

echo "==> Running test_frame_broker_ring"
"${BUILD_DIR}/test_frame_broker_ring" || FAIL=1

echo ""
if [ "${FAIL}" -eq 0 ]; then
    echo "===== ALL FRAME BROKER TESTS PASSED ====="
    exit 0
else
    echo "===== FRAME BROKER TESTS FAILED ====="
    exit 1
fi
