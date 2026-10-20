// Unit tests for FrameBufferPool (media frame buffer memory pool).
// Qt-free: compiles with plain g++ under -std=c++17.
//
// Build (single line):
//   g++ -std=c++17 -Wall -Wextra -pthread -I../../common -I../../media
//       tests/common/test_frame_buffer_pool.cpp
//       common/frame_buffer_pool.cpp
//       -o tests/common/test_frame_buffer_pool
#include "frame_buffer_pool.h"

#include <atomic>
#include <chrono>
#include <cstdio>
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

void testAcquireReturnsCorrectSize()
{
    pool::FrameBufferPool p(1024, /*minFree=*/2, /*max=*/8);
    auto block = p.acquire();
    CHECK(block != nullptr);
    CHECK(block->size() == 1024);
    const auto s = p.statistics();
    CHECK(s.capacityBytes == 1024);
    CHECK(s.totalAcquired == 1);
    // 首次 acquire 时 m_free 为空，应走 miss-allocated 路径。
    CHECK(s.totalMissAllocated == 1);
    CHECK(s.currentInUse == 1);
    CHECK(s.currentFree == 0);
}

void testReleaseReturnsToPool()
{
    pool::FrameBufferPool p(512, /*minFree=*/0, /*max=*/4);
    {
        auto block = p.acquire();
        CHECK(block->size() == 512);
        // 写入标识字节，验证归还后被复用。
        (*block)[0] = 0xAB;
        (*block)[511] = 0xCD;
        // 退出作用域 → shared_ptr 引用归零 → 自动归还。
    }
    const auto s = p.statistics();
    CHECK(s.currentInUse == 0);
    CHECK(s.currentFree == 1);
    CHECK(s.totalReturned == 1);

    // 再次 acquire 应命中空闲链表。
    auto block2 = p.acquire();
    CHECK(block2->size() == 512);
    const auto s2 = p.statistics();
    CHECK(s2.totalHit == 1);
    CHECK(s2.currentInUse == 1);
    CHECK(s2.currentFree == 0);
}

void testPrewarmCreatesFreeBlocks()
{
    pool::FrameBufferPool p(256, /*minFree=*/3, /*max=*/8);
    p.prewarm();
    const auto s = p.statistics();
    CHECK(s.currentFree == 3);
    CHECK(s.currentInUse == 0);
    // prewarm 不应计入 acquire 统计。
    CHECK(s.totalAcquired == 0);
}

void testSharedConstConversionCompatible()
{
    // 验证 shared_ptr<vector<uint8_t>> 可隐式转换为
    // shared_ptr<const vector<uint8_t>>（FramePacket::data 的类型）。
    pool::FrameBufferPool p(64, 0, 4);
    std::shared_ptr<std::vector<uint8_t>> block = p.acquire();
    std::shared_ptr<const std::vector<uint8_t>> constBlock = block;
    CHECK(constBlock->size() == 64);
    CHECK(constBlock.use_count() == 2);
}

void testMaxBlocksOverflowFallback()
{
    // max=2：借出 2 块达上限，第 3 次 acquire 应走 overflow 路径
    // 返回游离 buffer（不影响 currentFree）。
    pool::FrameBufferPool p(128, /*minFree=*/0, /*max=*/2);
    auto b1 = p.acquire();
    auto b2 = p.acquire();
    auto b3 = p.acquire();  // overflow: heap 分配，不归还到池
    const auto s = p.statistics();
    CHECK(s.totalOverflow == 1);
    CHECK(b3->size() == 128);  // 仍保证容量语义
    // b3 是游离块，引用归零时直接 delete，不会归还到池。
    (void)b1; (void)b2; (void)b3;
}

void testShrinkReleasesFreeBlocks()
{
    pool::FrameBufferPool p(256, /*minFree=*/4, /*max=*/8);
    p.prewarm();
    CHECK(p.statistics().currentFree == 4);
    const size_t freed = p.shrink();
    CHECK(freed == 4 * 256);
    CHECK(p.statistics().currentFree == 0);
}

void testConcurrentAcquireRelease()
{
    pool::FrameBufferPool p(64, /*minFree=*/4, /*max=*/0);  // 0 = 无上限
    p.prewarm();

    constexpr int kThreads = 4;
    constexpr int kIters = 500;
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&p, &errors]() {
            for (int i = 0; i < kIters; ++i) {
                auto block = p.acquire();
                if (!block || block->size() != 64) {
                    ++errors;
                    continue;
                }
                // 写入随机字节验证不串扰。
                (*block)[0] = static_cast<uint8_t>(i & 0xff);
                (*block)[63] = static_cast<uint8_t>((i >> 8) & 0xff);
                // 释放前验证内容仍可读。
                if ((*block)[0] != static_cast<uint8_t>(i & 0xff)) ++errors;
            }
        });
    }
    for (auto &t : threads) t.join();

    CHECK(errors.load() == 0);
    const auto s = p.statistics();
    CHECK(s.totalAcquired == static_cast<size_t>(kThreads * kIters));
    CHECK(s.totalReturned == static_cast<size_t>(kThreads * kIters));
    CHECK(s.currentInUse == 0);
}

} // namespace

int main()
{
    testAcquireReturnsCorrectSize();
    testReleaseReturnsToPool();
    testPrewarmCreatesFreeBlocks();
    testSharedConstConversionCompatible();
    testMaxBlocksOverflowFallback();
    testShrinkReleasesFreeBlocks();
    testConcurrentAcquireRelease();

    if (g_failures == 0) {
        std::puts("frame_buffer_pool tests passed");
        return 0;
    }
    std::printf("frame_buffer_pool tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
