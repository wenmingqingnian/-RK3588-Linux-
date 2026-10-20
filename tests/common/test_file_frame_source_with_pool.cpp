// Integration test: FileFrameSource + FrameBufferPool.
// Verifies the pool-backed frame source still produces correct NV12 bytes and
// that buffers are actually recycled through the pool (not heap-allocated
// every frame).
//
// Build (single line):
//   g++ -std=c++17 -Wall -Wextra -pthread -I../../common -I../../media
//       tests/common/test_file_frame_source_with_pool.cpp
//       common/frame_buffer_pool.cpp
//       media/file_frame_source.cpp
//       -o tests/common/test_file_frame_source_with_pool
#include "file_frame_source.h"
#include "frame_buffer_pool.h"
#include "frame_source.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
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

std::string writeTempNv12File(const std::string &path, int width, int height,
                              int frameCount)
{
    const size_t frameSize = static_cast<size_t>(width) * height * 3 / 2;
    std::vector<uint8_t> buf(frameSize * static_cast<size_t>(frameCount));
    for (int f = 0; f < frameCount; ++f) {
        uint8_t *frame = buf.data() + static_cast<size_t>(f) * frameSize;
        std::memset(frame, static_cast<uint8_t>(0x10 + f),
                    static_cast<size_t>(width) * height);
        std::memset(frame + static_cast<size_t>(width) * height, 0x80,
                    static_cast<size_t>(width) * height / 2);
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(buf.data()),
              static_cast<std::streamsize>(buf.size()));
    return path;
}

void testPoolBackedSourceReturnsCorrectBytes()
{
    constexpr int kW = 64;
    constexpr int kH = 32;
    constexpr int kFrames = 3;
    const std::string path = "tests/media/build/pool_integ_sample.nv12";
    writeTempNv12File(path, kW, kH, kFrames);

    const size_t frameSize = static_cast<size_t>(kW) * kH * 3 / 2;
    auto pool = std::make_shared<pool::FrameBufferPool>(
        frameSize, /*minFree=*/2, /*max=*/4);
    pool->prewarm();

    media::FileFrameSource src(path, kW, kH, /*fps=*/30, /*loop=*/false, pool);

    std::string err;
    CHECK(src.open(&err));
    CHECK(err.empty());

    for (int f = 0; f < kFrames; ++f) {
        media::FramePacket frame;
        const auto result = src.readFrame(&frame, /*timeoutMs=*/1000, &err);
        CHECK(result == media::FrameReadResult::FrameReady);
        CHECK(frame.isValid());
        CHECK(frame.width == kW);
        CHECK(frame.height == kH);
        CHECK(frame.format == media::PixelFormat::NV12);
        CHECK(frame.data->size() == frameSize);
        // Y plane sentinel = 0x10 + f
        CHECK((*frame.data)[0] == static_cast<uint8_t>(0x10 + f));
    }
    src.close();

    const auto s = pool->statistics();
    CHECK(s.capacityBytes == frameSize);
    CHECK(s.totalAcquired == static_cast<size_t>(kFrames));
    // pool 借出 + 归还次数应匹配。
    CHECK(s.totalReturned == static_cast<size_t>(kFrames));
    CHECK(s.currentInUse == 0);
    CHECK(s.currentFree >= 1);  // 至少有 1 块归还到池
    std::remove(path.c_str());
}

void testPoolBackedSourceRecyclesBuffers()
{
    constexpr int kW = 32;
    constexpr int kH = 16;
    const std::string path = "tests/media/build/pool_recycle_sample.nv12";
    writeTempNv12File(path, kW, kH, /*frameCount=*/10);

    const size_t frameSize = static_cast<size_t>(kW) * kH * 3 / 2;
    auto pool = std::make_shared<pool::FrameBufferPool>(
        frameSize, /*minFree=*/1, /*max=*/2);
    pool->prewarm();

    media::FileFrameSource src(path, kW, kH, /*fps=*/60, /*loop=*/true, pool);
    std::string err;
    CHECK(src.open(&err));

    // 读取 20 帧（loop=true 会回卷）。每帧读完立即释放 frame.data，
    // 故 pool.currentInUse 应在循环外始终 <= 1。
    for (int i = 0; i < 20; ++i) {
        media::FramePacket frame;
        const auto result = src.readFrame(&frame, 1000, &err);
        CHECK(result == media::FrameReadResult::FrameReady);
        CHECK(frame.data->size() == frameSize);
        // frame 在循环结束时析构 → buffer 归还到池
    }
    src.close();

    const auto s = pool->statistics();
    CHECK(s.totalAcquired == 20);
    CHECK(s.totalReturned == 20);
    CHECK(s.currentInUse == 0);
    // 关键断言：池命中次数应 > 0（证明 buffer 被回收复用）。
    CHECK(s.totalHit > 0);
    std::remove(path.c_str());
}

void testPoolSizeMismatchFallsBackToHeap()
{
    // 容量不匹配：pool.capacityBytes != frameSize，应静默回退 heap。
    constexpr int kW = 32;
    constexpr int kH = 16;
    const std::string path = "tests/media/build/pool_mismatch_sample.nv12";
    writeTempNv12File(path, kW, kH, 1);

    const size_t wrongSize = 8;  // 故意不匹配
    auto pool = std::make_shared<pool::FrameBufferPool>(
        wrongSize, /*minFree=*/1, /*max=*/2);

    media::FileFrameSource src(path, kW, kH, 30, false, pool);
    std::string err;
    CHECK(src.open(&err));

    media::FramePacket frame;
    const auto result = src.readFrame(&frame, 1000, &err);
    CHECK(result == media::FrameReadResult::FrameReady);
    CHECK(frame.data->size() == static_cast<size_t>(kW) * kH * 3 / 2);

    // pool 不应被借出。
    const auto s = pool->statistics();
    CHECK(s.totalAcquired == 0);
    src.close();
    std::remove(path.c_str());
}

} // namespace

int main()
{
    testPoolBackedSourceReturnsCorrectBytes();
    testPoolBackedSourceRecyclesBuffers();
    testPoolSizeMismatchFallsBackToHeap();

    if (g_failures == 0) {
        std::puts("file_frame_source_with_pool tests passed");
        return 0;
    }
    std::printf("file_frame_source_with_pool tests FAILED: %d failure(s)\n",
                g_failures);
    return 1;
}
