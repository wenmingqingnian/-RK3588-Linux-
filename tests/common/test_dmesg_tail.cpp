// Unit tests for DmesgTail (vehiclelog::DmesgTail).
// Qt-free: compiles with plain g++ under -std=c++17.
//
// Build (single line):
//   g++ -std=c++17 -Wall -Wextra -pthread -I../../common
//       tests/common/test_dmesg_tail.cpp
//       common/dmesg_tail.cpp
//       common/logmanager.cpp
//       -o tests/common/test_dmesg_tail
//
// Run:
//   ./tests/common/test_dmesg_tail
//
// 注：dmesg 集成测试以幂等/不崩溃为主，不验证内核日志内容
// （依赖系统权限与 dmesg 可用性）。
#include "dmesg_tail.h"
#include "logmanager.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

int g_failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// 基本契约：构造后未启动，isRunning() 为 false。
void testNewInstanceIsNotRunning()
{
    vehiclelog::DmesgTail tail;
    CHECK(!tail.isRunning());
}

// start() 后在 Linux 上应进入运行状态（若 dmesg 可用）。
// 在非 Linux 平台 start() 返回 false，isRunning() 仍为 false。
void testStartTransitionsState()
{
    vehiclelog::DmesgTail tail;
    bool ok = tail.start();
#ifdef __linux__
    // Linux 上 start 应返回 true（即使 dmesg 不可用也会启动线程再优雅退出）
    CHECK(ok);
    // 等待线程就绪
    for (int i = 0; i < 20 && !tail.isRunning(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
#else
    CHECK(!ok);
    CHECK(!tail.isRunning());
#endif
    tail.stop();
    CHECK(!tail.isRunning());
}

// 重复 start() 应返回 false（不允许多次启动同一实例）。
void testDoubleStartRejected()
{
    vehiclelog::DmesgTail tail;
    tail.start();
    bool second = tail.start();
    CHECK(!second);
    tail.stop();
}

// stop() 幂等：可重复调用不崩溃。
void testStopIsIdempotent()
{
    vehiclelog::DmesgTail tail;
    tail.stop();  // 未 start 时调用
    tail.stop();
    tail.start();
    tail.stop();
    tail.stop();
    CHECK(!tail.isRunning());
}

// 析构自动 stop：start 后不显式 stop，析构不应阻塞或崩溃。
void testDestructorStopsThread()
{
    {
        vehiclelog::DmesgTail tail;
        tail.start();
        // 不调用 stop
    } // 析构应在此完成
    CHECK(true); // 若能运行到此行说明析构未死锁
}

// start → stop → start 循环：可重启。
void testRestartAfterStop()
{
    vehiclelog::DmesgTail tail;
    tail.start();
    tail.stop();
    bool ok = tail.start();
#ifdef __linux__
    CHECK(ok);
#else
    CHECK(!ok);
#endif
    tail.stop();
}

} // namespace

int main()
{
    std::printf("[test] testNewInstanceIsNotRunning...\n"); std::fflush(stdout);
    testNewInstanceIsNotRunning();
    std::printf("[test] testStartTransitionsState...\n"); std::fflush(stdout);
    testStartTransitionsState();
    std::printf("[test] testDoubleStartRejected...\n"); std::fflush(stdout);
    testDoubleStartRejected();
    std::printf("[test] testStopIsIdempotent...\n"); std::fflush(stdout);
    testStopIsIdempotent();
    std::printf("[test] testDestructorStopsThread...\n"); std::fflush(stdout);
    testDestructorStopsThread();
    std::printf("[test] testRestartAfterStop...\n"); std::fflush(stdout);
    testRestartAfterStop();

    if (g_failures == 0) {
        std::printf("test_dmesg_tail: ALL PASSED\n");
        return 0;
    }
    std::printf("test_dmesg_tail: %d FAILURE(S)\n", g_failures);
    return 1;
}
