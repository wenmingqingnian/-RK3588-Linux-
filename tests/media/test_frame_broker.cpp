// Unit tests for FrameBroker (3 bounded lanes with drop_oldest).
// Qt-free: compiles with plain g++ under -std=c++17.
#include "frame_broker.h"

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

media::FramePacket makeFrame(uint64_t seq)
{
    media::FramePacket f;
    auto buf = std::make_shared<std::vector<uint8_t>>(4);
    (*buf)[0] = static_cast<uint8_t>(seq & 0xff);
    f.data = buf;
    f.width = 160;
    f.height = 120;
    f.format = media::PixelFormat::NV12;
    f.sequence = seq;
    f.timestampUs = seq * 1000;
    return f;
}

void testPublishAndTakeReturnsFrame()
{
    media::FrameBroker broker(3);
    media::FramePacket f = makeFrame(1);
    CHECK(broker.publish(f) == 3); // all 3 lanes accept

    media::FramePacket out;
    CHECK(broker.take(media::ConsumerLane::Gui, &out, 1000) ==
          media::FrameReadResult::FrameReady);
    CHECK(out.sequence == 1);
}

void testTakeOnEmptyTimesOut()
{
    media::FrameBroker broker(3);
    media::FramePacket out;
    auto start = std::chrono::steady_clock::now();
    CHECK(broker.take(media::ConsumerLane::Gui, &out, 100) ==
          media::FrameReadResult::Timeout);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start).count();
    CHECK(elapsed >= 80); // allow small scheduling slack
}

void testQueueFullDropsOldest()
{
    media::FrameBroker broker(2); // capacity 2
    broker.setLaneEnabled(media::ConsumerLane::Rknn, false);
    broker.setLaneEnabled(media::ConsumerLane::Rtsp, false);

    broker.publish(makeFrame(1));
    broker.publish(makeFrame(2));
    broker.publish(makeFrame(3)); // should drop frame 1

    media::FramePacket out;
    CHECK(broker.take(media::ConsumerLane::Gui, &out, 100) ==
          media::FrameReadResult::FrameReady);
    CHECK(out.sequence == 2); // oldest was dropped
    CHECK(broker.droppedCount(media::ConsumerLane::Gui) == 1);
}

void testQueueDepthNeverExceedsCapacity()
{
    media::FrameBroker broker(3);
    broker.setLaneEnabled(media::ConsumerLane::Rknn, false);
    broker.setLaneEnabled(media::ConsumerLane::Rtsp, false);

    for (int i = 0; i < 10; ++i)
        broker.publish(makeFrame(static_cast<uint64_t>(i)));

    CHECK(broker.queueDepth(media::ConsumerLane::Gui) <= 3);
    CHECK(broker.droppedCount(media::ConsumerLane::Gui) == 7);
}

void testDisabledLaneDropsFrames()
{
    media::FrameBroker broker(3);
    broker.setLaneEnabled(media::ConsumerLane::Rknn, false);
    broker.setLaneEnabled(media::ConsumerLane::Rtsp, false);

    broker.publish(makeFrame(1));
    // Disabled lanes should not buffer.
    media::FramePacket out;
    CHECK(broker.take(media::ConsumerLane::Rknn, &out, 100) ==
          media::FrameReadResult::Timeout);
}

void testRequestStopUnblocksTake()
{
    media::FrameBroker broker(3);

    std::thread t([&broker]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        broker.requestStop();
    });

    media::FramePacket out;
    auto start = std::chrono::steady_clock::now();
    CHECK(broker.take(media::ConsumerLane::Gui, &out, 5000) ==
          media::FrameReadResult::Stopped);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start).count();
    CHECK(elapsed < 1000); // must return promptly after stop
    t.join();
}

void testLanesAreIndependent()
{
    media::FrameBroker broker(3);
    broker.setLaneEnabled(media::ConsumerLane::Rknn, false);
    broker.setLaneEnabled(media::ConsumerLane::Rtsp, false);

    broker.publish(makeFrame(1));
    broker.publish(makeFrame(2));

    // GUI lane has 2 frames; only take one. Publish another. GUI should
    // have 3 (under capacity) while Rknn/Rtsp remain empty.
    media::FramePacket gui1;
    broker.take(media::ConsumerLane::Gui, &gui1, 100);
    broker.publish(makeFrame(3));
    CHECK(broker.queueDepth(media::ConsumerLane::Gui) == 2);
}

void testResetClearsState()
{
    media::FrameBroker broker(2);
    broker.publish(makeFrame(1));
    broker.publish(makeFrame(2));
    broker.publish(makeFrame(3)); // drop 1

    broker.reset();

    CHECK(broker.queueDepth(media::ConsumerLane::Gui) == 0);
    CHECK(broker.droppedCount(media::ConsumerLane::Gui) == 0);
    // After reset, take should timeout (not Stopped).
    media::FramePacket out;
    CHECK(broker.take(media::ConsumerLane::Gui, &out, 50) ==
          media::FrameReadResult::Timeout);
}

} // namespace

int main()
{
    testPublishAndTakeReturnsFrame();
    testTakeOnEmptyTimesOut();
    testQueueFullDropsOldest();
    testQueueDepthNeverExceedsCapacity();
    testDisabledLaneDropsFrames();
    testRequestStopUnblocksTake();
    testLanesAreIndependent();
    testResetClearsState();

    if (g_failures == 0) {
        std::puts("frame_broker tests passed");
        return 0;
    }
    std::printf("frame_broker tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
