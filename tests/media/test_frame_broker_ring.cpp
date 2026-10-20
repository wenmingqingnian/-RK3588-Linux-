// Unit tests for FrameBroker's fixed-capacity ring buffer implementation.
// Focuses on edge cases that the original std::queue-based implementation
// could not exercise: ring wrap-around, move semantics correctness, slot
// reuse, and high-throughput stress.
//
// Qt-free: compiles with plain g++ under -std=c++17.
//
// Build (single line):
//   g++ -std=c++17 -Wall -Wextra -pthread -I../../media
//       tests/media/test_frame_broker_ring.cpp
//       media/frame_broker.cpp
//       -o tests/media/test_frame_broker_ring
#include "frame_broker.h"

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

media::FramePacket makeFrame(uint64_t seq, uint8_t marker = 0)
{
    media::FramePacket f;
    auto buf = std::make_shared<std::vector<uint8_t>>(8);
    (*buf)[0] = static_cast<uint8_t>(seq & 0xff);
    (*buf)[1] = static_cast<uint8_t>((seq >> 8) & 0xff);
    (*buf)[2] = marker;
    f.data = buf;
    f.width = 160;
    f.height = 120;
    f.format = media::PixelFormat::NV12;
    f.sequence = seq;
    f.timestampUs = seq * 1000;
    return f;
}

// Capacity 1 is the tightest ring: every publish after the first must drop.
void testCapacityOneRing()
{
    std::printf("RUN  capacity-1 ring: every publish drops\n");
    media::FrameBroker broker(1);
    broker.setLaneEnabled(media::ConsumerLane::Rknn, false);
    broker.setLaneEnabled(media::ConsumerLane::Rtsp, false);

    CHECK(broker.publish(makeFrame(1)) == 1);
    CHECK(broker.publish(makeFrame(2)) == 1);  // drops frame 1
    CHECK(broker.publish(makeFrame(3)) == 1);  // drops frame 2

    media::FramePacket out;
    CHECK(broker.take(media::ConsumerLane::Gui, &out, 100) ==
          media::FrameReadResult::FrameReady);
    CHECK(out.sequence == 3);  // latest retained
    CHECK(broker.droppedCount(media::ConsumerLane::Gui) == 2);
    CHECK(broker.queueDepth(media::ConsumerLane::Gui) == 0);
}

// Fill-and-drain several cycles to force head/tail to wrap around multiple
// times. This is the case std::queue could not exercise.
void testRingWrapAroundMultipleCycles()
{
    std::printf("RUN  ring wrap-around over multiple fill/drain cycles\n");
    media::FrameBroker broker(3);
    broker.setLaneEnabled(media::ConsumerLane::Rknn, false);
    broker.setLaneEnabled(media::ConsumerLane::Rtsp, false);

    // 5 cycles of: fill to capacity, drain all. With capacity 3, this
    // exercises 15 publish + 15 take operations, forcing head and tail to
    // wrap around the ring 5 times each.
    for (int cycle = 0; cycle < 5; ++cycle) {
        const uint64_t base = static_cast<uint64_t>(cycle) * 10;
        broker.publish(makeFrame(base + 1));
        broker.publish(makeFrame(base + 2));
        broker.publish(makeFrame(base + 3));
        CHECK(broker.queueDepth(media::ConsumerLane::Gui) == 3);

        for (int i = 1; i <= 3; ++i) {
            media::FramePacket out;
            CHECK(broker.take(media::ConsumerLane::Gui, &out, 100) ==
                  media::FrameReadResult::FrameReady);
            CHECK(out.sequence == base + static_cast<uint64_t>(i));
        }
        CHECK(broker.queueDepth(media::ConsumerLane::Gui) == 0);
    }
    // No drops should have occurred in any cycle.
    CHECK(broker.droppedCount(media::ConsumerLane::Gui) == 0);
}

// Verify that take() actually moves the shared_ptr out of the slot (the
// slot's shared_ptr is reset, so the underlying buffer's refcount drops).
void testTakeMovesFrameOutOfSlot()
{
    std::printf("RUN  take() moves frame out of slot (slot resets)\n");
    media::FrameBroker broker(3);
    broker.setLaneEnabled(media::ConsumerLane::Rknn, false);
    broker.setLaneEnabled(media::ConsumerLane::Rtsp, false);

    auto buf = std::make_shared<std::vector<uint8_t>>(8);
    (*buf)[0] = 42;
    // Reference holders: `buf` itself (1) + `f` below (1) = 2 baseline.
    // publish() takes const FramePacket& so it COPIES into the slot (+1).
    media::FramePacket f;
    f.data = buf;
    f.width = 160;
    f.height = 120;
    f.format = media::PixelFormat::NV12;
    f.sequence = 1;
    const long baseline = 2;  // buf + f
    CHECK(buf.use_count() == baseline);
    broker.publish(f);
    // Now: buf + f + slot = 3
    CHECK(buf.use_count() == baseline + 1);

    media::FramePacket out;
    CHECK(broker.take(media::ConsumerLane::Gui, &out, 100) ==
          media::FrameReadResult::FrameReady);
    // After take: buf + f + out = 3 (slot released its ref via move, but
    // `out` now holds a ref). The slot's shared_ptr was moved into `out`,
    // so the count stays the same — but the KEY point is the slot no longer
    // holds a ref. Verify by dropping `f`'s ref and checking count.
    CHECK(buf.use_count() == baseline + 1);
    f.data.reset();  // drop f's ref
    CHECK(buf.use_count() == baseline);  // now: buf + out = 2
    CHECK((*out.data)[0] == 42);
}

// After a drop, the dropped slot's shared_ptr must be released (not leaked).
void testDroppedSlotReleasesBuffer()
{
    std::printf("RUN  dropped slot releases its buffer reference\n");
    media::FrameBroker broker(1);
    broker.setLaneEnabled(media::ConsumerLane::Rknn, false);
    broker.setLaneEnabled(media::ConsumerLane::Rtsp, false);

    auto buf1 = std::make_shared<std::vector<uint8_t>>(4);
    auto buf2 = std::make_shared<std::vector<uint8_t>>(4);
    // Baseline: each buffer held only by its local variable = 1.

    media::FramePacket f1; f1.data = buf1; f1.width = 1; f1.height = 1;
    f1.format = media::PixelFormat::NV12;
    media::FramePacket f2; f2.data = buf2; f2.width = 1; f2.height = 1;
    f2.format = media::PixelFormat::NV12;

    broker.publish(f1);  // buf1: local + f1 + slot = 3
    CHECK(buf1.use_count() == 3);
    broker.publish(f2);  // drops f1's slot, overwrites with f2
    // buf1's refs: local + f1 = 2 (slot released via FramePacket{} assign)
    CHECK(buf1.use_count() == 2);
    // buf2's refs: local + f2 + slot = 3
    CHECK(buf2.use_count() == 3);

    media::FramePacket out;
    broker.take(media::ConsumerLane::Gui, &out, 100);
    CHECK(out.data == buf2);
    // Now buf2: local + f2 + out = 3 (slot released via move to out)
    CHECK(buf2.use_count() == 3);
}

// setLaneEnabled(false) must release all buffered frames in that lane.
void testDisableLaneReleasesAllBuffers()
{
    std::printf("RUN  disabling a lane releases all its buffered frames\n");
    media::FrameBroker broker(3);
    broker.setLaneEnabled(media::ConsumerLane::Rknn, false);
    broker.setLaneEnabled(media::ConsumerLane::Rtsp, false);

    auto b1 = std::make_shared<std::vector<uint8_t>>(4);
    auto b2 = std::make_shared<std::vector<uint8_t>>(4);
    // Baseline: each held only by local var = 1.

    media::FramePacket f1; f1.data = b1; f1.width = 1; f1.height = 1;
    f1.format = media::PixelFormat::NV12;
    media::FramePacket f2; f2.data = b2; f2.width = 1; f2.height = 1;
    f2.format = media::PixelFormat::NV12;
    broker.publish(f1);
    broker.publish(f2);
    // b1: local + f1 + slot = 3; b2: local + f2 + slot = 3
    CHECK(b1.use_count() == 3);
    CHECK(b2.use_count() == 3);

    broker.setLaneEnabled(media::ConsumerLane::Gui, false);
    // Slots released: b1: local + f1 = 2; b2: local + f2 = 2
    CHECK(b1.use_count() == 2);
    CHECK(b2.use_count() == 2);
    CHECK(broker.queueDepth(media::ConsumerLane::Gui) == 0);
}

// reset() must release all buffered frames.
void testResetReleasesAllBuffers()
{
    std::printf("RUN  reset() releases all buffered frames\n");
    media::FrameBroker broker(3);
    broker.setLaneEnabled(media::ConsumerLane::Rknn, false);
    broker.setLaneEnabled(media::ConsumerLane::Rtsp, false);

    auto b1 = std::make_shared<std::vector<uint8_t>>(4);
    auto b2 = std::make_shared<std::vector<uint8_t>>(4);
    // Baseline: each held only by local var = 1.

    media::FramePacket f1; f1.data = b1; f1.width = 1; f1.height = 1;
    f1.format = media::PixelFormat::NV12;
    media::FramePacket f2; f2.data = b2; f2.width = 1; f2.height = 1;
    f2.format = media::PixelFormat::NV12;
    broker.publish(f1);
    broker.publish(f2);

    broker.reset();
    // b1: local + f1 = 2; b2: local + f2 = 2 (slots released)
    CHECK(b1.use_count() == 2);
    CHECK(b2.use_count() == 2);
}

// Stress test: 1 producer + 3 consumers, many frames. Verifies no corruption,
// no lost wakeups, no deadlock, and FIFO order per lane.
void testStressProducerThreeConsumers()
{
    std::printf("RUN  stress: 1 producer + 3 consumers, 500 frames\n");
    constexpr int N = 500;
    media::FrameBroker broker(4);

    std::atomic<int> received[3] = {{0}, {0}, {0}};
    std::atomic<bool> producerDone{false};

    auto consumer = [&broker, &received, N](media::ConsumerLane lane,
                                            int idx) {
        uint64_t expectedSeq = 0;
        while (true) {
            media::FramePacket f;
            const auto r = broker.take(lane, &f, 1000);
            if (r == media::FrameReadResult::Stopped)
                break;
            if (r == media::FrameReadResult::FrameReady) {
                // Frames may be dropped under load, but sequence must never
                // go backwards (FIFO order preserved per lane).
                if (f.sequence >= expectedSeq) {
                    expectedSeq = f.sequence + 1;
                } else {
                    std::printf("FAIL: lane %d got seq %llu < expected %llu\n",
                                idx,
                                static_cast<unsigned long long>(f.sequence),
                                static_cast<unsigned long long>(expectedSeq));
                    ++g_failures;
                }
                received[idx].fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::thread consumers[3] = {
        std::thread(consumer, media::ConsumerLane::Gui, 0),
        std::thread(consumer, media::ConsumerLane::Rknn, 1),
        std::thread(consumer, media::ConsumerLane::Rtsp, 2),
    };

    for (int i = 0; i < N; ++i)
        broker.publish(makeFrame(static_cast<uint64_t>(i)));
    producerDone.store(true);

    // Wait for consumers to drain, then stop.
    while (received[0].load() + received[1].load() + received[2].load() < 3)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    broker.requestStop();
    for (auto &t : consumers)
        t.join();

    // Each lane should have received at least one frame (the last published).
    CHECK(received[0].load() > 0);
    CHECK(received[1].load() > 0);
    CHECK(received[2].load() > 0);
    // No lane should receive more than N frames.
    CHECK(received[0].load() <= N);
    CHECK(received[1].load() <= N);
    CHECK(received[2].load() <= N);
}

// Stress test: rapid enable/disable cycling under publish. Verifies no
// crashes and consistent state after settling.
void testStressEnableDisableCycling()
{
    std::printf("RUN  stress: rapid enable/disable cycling under publish\n");
    media::FrameBroker broker(2);

    std::atomic<bool> stop{false};
    std::atomic<int> publishCount{0};

    std::thread toggler([&broker, &stop]() {
        for (int i = 0; i < 1000 && !stop.load(); ++i) {
            broker.setLaneEnabled(media::ConsumerLane::Gui, (i % 2) == 0);
        }
    });
    std::thread publisher([&broker, &stop, &publishCount]() {
        for (int i = 0; i < 1000 && !stop.load(); ++i) {
            broker.publish(makeFrame(static_cast<uint64_t>(i)));
            publishCount.fetch_add(1);
        }
    });
    toggler.join();
    publisher.join();
    stop.store(true);
    broker.requestStop();

    // After settling, queueDepth must be within capacity.
    CHECK(broker.queueDepth(media::ConsumerLane::Gui) <= 2);
    CHECK(broker.queueDepth(media::ConsumerLane::Rknn) <= 2);
    CHECK(broker.queueDepth(media::ConsumerLane::Rtsp) <= 2);
    CHECK(publishCount.load() == 1000);
}

} // namespace

int main()
{
    testCapacityOneRing();
    testRingWrapAroundMultipleCycles();
    testTakeMovesFrameOutOfSlot();
    testDroppedSlotReleasesBuffer();
    testDisableLaneReleasesAllBuffers();
    testResetReleasesAllBuffers();
    testStressProducerThreeConsumers();
    testStressEnableDisableCycling();

    if (g_failures == 0) {
        std::puts("frame_broker_ring tests passed");
        return 0;
    }
    std::printf("frame_broker_ring tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
