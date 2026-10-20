// Unit tests for ThreadPool (concurrency::ThreadPool).
// Qt-free: compiles with plain g++ under -std=c++17.
//
// Build (single line):
//   g++ -std=c++17 -Wall -Wextra -pthread -I../../common
//       tests/common/test_thread_pool.cpp
//       common/thread_pool.cpp
//       -o tests/common/test_thread_pool
#include "thread_pool.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

void testSubmitReturnsFutureWithValue()
{
    concurrency::ThreadPool pool(2);
    auto fut = pool.submit([]() { return 42; });
    CHECK(fut.get() == 42);

    const auto s = pool.statistics();
    CHECK(s.workerCount == 2);
    CHECK(s.submittedTasks >= 1);
    CHECK(s.completedTasks >= 1);
}

void testSubmitWithArguments()
{
    concurrency::ThreadPool pool(2);
    auto fut = pool.submit([](int a, int b) { return a + b; }, 3, 4);
    CHECK(fut.get() == 7);
}

void testSubmitMultipleTasksRunInParallel()
{
    // 2 worker 并行 sleep 100ms，总耗时应 < 200ms（串行会 >= 400ms）。
    concurrency::ThreadPool pool(2);
    const auto t0 = std::chrono::steady_clock::now();
    auto f1 = pool.submit([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return 1;
    });
    auto f2 = pool.submit([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return 2;
    });
    CHECK(f1.get() == 1);
    CHECK(f2.get() == 2);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count();
    CHECK(elapsed < 300);  // 留余量，允许 250ms 上下
}

void testExceptionPropagatesThroughFuture()
{
    concurrency::ThreadPool pool(1);
    auto fut = pool.submit([]() -> int {
        throw std::runtime_error("boom");
        return 0;
    });
    bool caught = false;
    try {
        (void)fut.get();
    } catch (const std::runtime_error &e) {
        caught = true;
        CHECK(std::string(e.what()) == "boom");
    }
    CHECK(caught);
}

void testWaitIdleBlocksUntilAllDone()
{
    concurrency::ThreadPool pool(2);
    std::atomic<int> counter{0};
    for (int i = 0; i < 10; ++i) {
        pool.submit([&counter]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ++counter;
        });
    }
    pool.waitIdle();
    CHECK(counter.load() == 10);
    const auto s = pool.statistics();
    CHECK(s.pendingTasks == 0);
    CHECK(s.activeWorkers == 0);
    CHECK(s.completedTasks == 10);
}

void testStopMakesSubmitReturnBrokenFuture()
{
    concurrency::ThreadPool pool(1);
    pool.stop();
    auto fut = pool.submit([]() { return 1; });
    bool caught = false;
    try {
        (void)fut.get();
    } catch (const std::runtime_error &) {
        caught = true;
    }
    CHECK(caught);
}

void testStopIsIdempotent()
{
    concurrency::ThreadPool pool(2);
    pool.stop();
    pool.stop();  // 重复调用不应崩溃。
    CHECK(pool.statistics().workerCount == 0);
}

void testDestructorJoinsAllWorkers()
{
    // 析构时仍有任务在跑：必须 join 完成。
    std::atomic<int> counter{0};
    {
        concurrency::ThreadPool pool(4);
        for (int i = 0; i < 20; ++i) {
            pool.submit([&counter]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                ++counter;
            });
        }
        // 不显式 stop，直接析构。
    }
    CHECK(counter.load() == 20);
}

void testConcurrencyStress()
{
    concurrency::ThreadPool pool(4);
    constexpr int kTasks = 500;
    std::atomic<int> sum{0};
    std::vector<std::future<int>> futs;
    futs.reserve(kTasks);
    for (int i = 0; i < kTasks; ++i) {
        futs.push_back(pool.submit([i]() { return i; }));
    }
    for (auto &f : futs) sum += f.get();
    // 0+1+...+(kTasks-1) = (kTasks-1)*kTasks/2
    CHECK(sum.load() == (kTasks - 1) * kTasks / 2);
}

} // namespace

int main()
{
    testSubmitReturnsFutureWithValue();
    testSubmitWithArguments();
    testSubmitMultipleTasksRunInParallel();
    testExceptionPropagatesThroughFuture();
    testWaitIdleBlocksUntilAllDone();
    testStopMakesSubmitReturnBrokenFuture();
    testStopIsIdempotent();
    testDestructorJoinsAllWorkers();
    testConcurrencyStress();

    if (g_failures == 0) {
        std::puts("thread_pool tests passed");
        return 0;
    }
    std::printf("thread_pool tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
