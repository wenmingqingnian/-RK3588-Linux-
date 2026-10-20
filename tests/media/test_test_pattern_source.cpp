// Unit tests for TestPatternSource (NV12 test pattern generator).
// Qt-free: compiles with plain g++ under -std=c++17.
#include "test_pattern_source.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
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

void testOpenSucceedsWithValidConfig()
{
    media::TestPatternSource source(320, 240, 25);
    std::string error;
    CHECK(source.open(&error));
    source.close();
}

void testReadFrameProducesValidNv12Frame()
{
    media::TestPatternSource source(320, 240, 25);
    std::string error;
    CHECK(source.open(&error));

    media::FramePacket frame;
    std::string readError;
    media::FrameReadResult result = source.readFrame(&frame, 1000, &readError);
    CHECK(result == media::FrameReadResult::FrameReady);
    CHECK(frame.isValid());
    CHECK(frame.width == 320);
    CHECK(frame.height == 240);
    CHECK(frame.format == media::PixelFormat::NV12);
    const size_t expectedSize = static_cast<size_t>(320) * 240 * 3 / 2;
    CHECK(frame.data->size() == expectedSize);
    source.close();
}

void testSequenceIsMonotonicallyIncreasing()
{
    media::TestPatternSource source(160, 120, 60);
    std::string error;
    CHECK(source.open(&error));

    media::FramePacket f1, f2, f3;
    std::string readError;
    CHECK(source.readFrame(&f1, 1000, &readError) == media::FrameReadResult::FrameReady);
    CHECK(source.readFrame(&f2, 1000, &readError) == media::FrameReadResult::FrameReady);
    CHECK(source.readFrame(&f3, 1000, &readError) == media::FrameReadResult::FrameReady);
    CHECK(f2.sequence == f1.sequence + 1);
    CHECK(f3.sequence == f2.sequence + 1);
    CHECK(f2.timestampUs >= f1.timestampUs);
    CHECK(f3.timestampUs >= f2.timestampUs);
    source.close();
}

void testNv12YPlaneIsNonZero()
{
    media::TestPatternSource source(160, 120, 60);
    std::string error;
    CHECK(source.open(&error));

    media::FramePacket frame;
    std::string readError;
    CHECK(source.readFrame(&frame, 1000, &readError) == media::FrameReadResult::FrameReady);
    // Color bars must produce non-zero Y values (not pure black frame).
    bool anyNonZero = false;
    const uint8_t *y = frame.data->data();
    for (size_t i = 0; i < static_cast<size_t>(160) * 120; ++i) {
        if (y[i] != 0) { anyNonZero = true; break; }
    }
    CHECK(anyNonZero);
    source.close();
}

void testRequestStopUnblocksPendingRead()
{
    media::TestPatternSource source(160, 120, 1); // 1 fps -> 1000ms pacing
    std::string error;
    CHECK(source.open(&error));

    media::FramePacket frame;
    std::string readError;
    // Read one frame first.
    CHECK(source.readFrame(&frame, 2000, &readError) == media::FrameReadResult::FrameReady);

    // Now request stop from another thread while a read is pending.
    std::atomic<bool> readReturned{false};
    std::thread reader([&]() {
        media::FramePacket f;
        std::string e;
        media::FrameReadResult r = source.readFrame(&f, 5000, &e);
        CHECK(r == media::FrameReadResult::Stopped);
        readReturned = true;
    });

    // Give the reader a moment to enter the wait, then stop.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    source.requestStop();
    // Must return well under the 5s timeout.
    reader.join();
    CHECK(readReturned);
    source.close();
}

void testCloseIsIdempotentAndAllowsReopen()
{
    media::TestPatternSource source(160, 120, 25);
    std::string error;
    CHECK(source.open(&error));
    source.close();
    source.close(); // idempotent
    std::string error2;
    CHECK(source.open(&error2)); // reopen
    media::FramePacket frame;
    std::string readError;
    CHECK(source.readFrame(&frame, 1000, &readError) == media::FrameReadResult::FrameReady);
    source.close();
}

void testInvalidDimensionsRejected()
{
    media::TestPatternSource source(0, 120, 25);
    std::string error;
    CHECK(!source.open(&error));
    CHECK(!error.empty());

    media::TestPatternSource source2(161, 0, 25); // odd width invalid for NV12
    std::string error2;
    CHECK(!source2.open(&error2));
    CHECK(!error2.empty());
}

void testReadBeforeOpenReturnsFatalError()
{
    media::TestPatternSource source(160, 120, 25);
    media::FramePacket frame;
    std::string error;
    media::FrameReadResult r = source.readFrame(&frame, 100, &error);
    CHECK(r == media::FrameReadResult::FatalError);
}

} // namespace

int main()
{
    testOpenSucceedsWithValidConfig();
    testReadFrameProducesValidNv12Frame();
    testSequenceIsMonotonicallyIncreasing();
    testNv12YPlaneIsNonZero();
    testRequestStopUnblocksPendingRead();
    testCloseIsIdempotentAndAllowsReopen();
    testInvalidDimensionsRejected();
    testReadBeforeOpenReturnsFatalError();

    if (g_failures == 0) {
        std::puts("test_pattern_source tests passed");
        return 0;
    }
    std::printf("test_pattern_source tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
