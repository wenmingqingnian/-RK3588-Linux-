#!/bin/bash
# build_and_run_rknn_tests.sh
# ----------------------------------------------------------------------------
# Build and run the RKNN postprocess tests on the board (Linux, no NPU
# required — these are pure CPU host tests).
#
# Tests covered:
#   1. test_rknn_postprocess_adapter  — existing 9-scenario contract tests
#                                       (regression baseline; verifies the
#                                       refactor of decodeOutputs() preserved
#                                       behavior)
#   2. test_rknn_postprocess_parallel — new 8-scenario equivalence tests
#                                       (verifies decodeOutputsParallel()
#                                       produces byte-for-byte identical
#                                       results to decodeOutputs(), including
#                                       a 200-iteration stress test)
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
# Test 1: existing serial adapter contract tests (regression baseline)
# ---------------------------------------------------------------------------
echo "==> Building test_rknn_postprocess_adapter"
g++ -std=c++17 -Wall -Wextra -pthread \
    -I "${PROJECT_ROOT}/media" \
    -I "${PROJECT_ROOT}/common" \
    -DRKNN_TEST_PROJECT_ROOT="\"${PROJECT_ROOT}\"" \
    "${SCRIPT_DIR}/test_rknn_postprocess_adapter.cpp" \
    "${PROJECT_ROOT}/media/rknn_postprocess.cpp" \
    "${PROJECT_ROOT}/common/thread_pool.cpp" \
    -o "${BUILD_DIR}/test_rknn_postprocess_adapter" 2>&1

echo "==> Running test_rknn_postprocess_adapter"
"${BUILD_DIR}/test_rknn_postprocess_adapter" || FAIL=1

# ---------------------------------------------------------------------------
# Test 2: new parallel equivalence tests
# ---------------------------------------------------------------------------
echo "==> Building test_rknn_postprocess_parallel"
g++ -std=c++17 -Wall -Wextra -pthread \
    -I "${PROJECT_ROOT}/media" \
    -I "${PROJECT_ROOT}/common" \
    "${SCRIPT_DIR}/test_rknn_postprocess_parallel.cpp" \
    "${PROJECT_ROOT}/media/rknn_postprocess.cpp" \
    "${PROJECT_ROOT}/common/thread_pool.cpp" \
    -o "${BUILD_DIR}/test_rknn_postprocess_parallel" 2>&1

echo "==> Running test_rknn_postprocess_parallel"
"${BUILD_DIR}/test_rknn_postprocess_parallel" || FAIL=1

echo ""
if [ "${FAIL}" -eq 0 ]; then
    echo "===== ALL RKNN POSTPROCESS TESTS PASSED ====="
    exit 0
else
    echo "===== RKNN POSTPROCESS TESTS FAILED ====="
    exit 1
fi
