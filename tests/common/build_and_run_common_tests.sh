#!/bin/bash
# build_and_run_common_tests.sh
#
# 构建 + 运行 common/ 下的内存池与线程池单元测试。
# 不依赖 Qt、不依赖板端硬件，可在开发机或 RK3588S 板端执行。
#
# Usage:
#   ./tests/common/build_and_run_common_tests.sh
#
# 退出码:
#   0 全部测试通过
#   1 编译失败或有测试失败

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
TEST_DIR="$PROJECT_DIR/tests/common"
BUILD_DIR="$TEST_DIR/build"

CXX="${CXX:-g++}"
CXXFLAGS="-std=c++17 -Wall -Wextra -Wno-unused-parameter -pthread"

mkdir -p "$BUILD_DIR"

fail=0

build_and_run() {
    local name="$1"
    local test_src="$2"
    shift 2
    local sources=("$@")

    local out="$BUILD_DIR/$name"
    echo "==> Building $name"
    if ! $CXX $CXXFLAGS -I "$PROJECT_DIR/common" -I "$PROJECT_DIR/media" \
            "$TEST_DIR/$test_src" "${sources[@]/#/$PROJECT_DIR/}" \
            -o "$out"; then
        echo "!!  Build failed: $name"
        fail=1
        return
    fi

    echo "==> Running $name"
    if ! "$out"; then
        echo "!!  Test failed: $name"
        fail=1
    fi
}

# 1. FrameBufferPool 单元测试
build_and_run test_frame_buffer_pool \
    test_frame_buffer_pool.cpp \
    common/frame_buffer_pool.cpp

# 2. ThreadPool 单元测试
build_and_run test_thread_pool \
    test_thread_pool.cpp \
    common/thread_pool.cpp

# 3. FileFrameSource + Pool 集成测试
build_and_run test_file_frame_source_with_pool \
    test_file_frame_source_with_pool.cpp \
    common/frame_buffer_pool.cpp \
    media/file_frame_source.cpp

if [ "$fail" -ne 0 ]; then
    echo ""
    echo "===== SOME TESTS FAILED ====="
    exit 1
fi

echo ""
echo "===== ALL COMMON TESTS PASSED ====="
exit 0
