// Unit tests for FramePacket ownership and validation semantics.
// Qt-free: compiles with plain g++ under -std=c++17.
#include "frame_packet.h"

#include <cstdio>
#include <shared_mutex>
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

void testDefaultFramePacketIsInvalid()
{
    media::FramePacket frame;
    CHECK(!frame.isValid());
    CHECK(frame.width == 0);
    CHECK(frame.height == 0);
    CHECK(frame.format == media::PixelFormat::Unknown);
    CHECK(frame.sequence == 0);
    CHECK(frame.timestampUs == 0);
    CHECK(frame.dmabufFd == -1);
    CHECK(frame.data == nullptr);
}

void testValidFramePacketReportsValid()
{
    auto buffer = std::make_shared<std::vector<uint8_t>>(100);
    media::FramePacket frame;
    frame.data = buffer;
    frame.width = 320;
    frame.height = 240;
    frame.horizontalStride = 320;
    frame.verticalStride = 240;
    frame.format = media::PixelFormat::NV12;
    frame.sequence = 7;
    frame.timestampUs = 12345;

    CHECK(frame.isValid());
}

void testSharedOwnershipKeepsBufferAliveAfterProducerRelease()
{
    std::weak_ptr<const std::vector<uint8_t>> weak;
    media::FramePacket frame;
    {
        auto buffer = std::make_shared<std::vector<uint8_t>>(50);
        weak = buffer;
        frame.data = buffer;
        buffer.reset();
        // Producer released its reference, but frame still holds one.
        CHECK(!weak.expired());
        CHECK(frame.data->size() == 50);
    }
    // frame still alive
    CHECK(!weak.expired());
    CHECK(frame.data->size() == 50);
    frame.data.reset();
    CHECK(weak.expired());
}

void testFramePacketIsImmutableThroughSharedConst()
{
    auto buffer = std::make_shared<std::vector<uint8_t>>(4);
    (*buffer)[0] = 0xAB;
    media::FramePacket frame;
    frame.data = buffer;
    // data is shared_ptr<const vector>, so contents are read-only.
    // Verify the byte is readable and unchanged.
    CHECK((*frame.data)[0] == 0xAB);
    // Two frames can share the same buffer (zero-copy between consumers).
    media::FramePacket frame2 = frame;
    CHECK(frame2.data.get() == frame.data.get());
    CHECK(frame2.data.use_count() == 3); // buffer + frame + frame2
}

void testNv12BufferSizeCalculation()
{
    const int w = 800;
    const int h = 600;
    const size_t expected = static_cast<size_t>(w) * h * 3 / 2;
    auto buffer = std::make_shared<std::vector<uint8_t>>(expected);
    media::FramePacket frame;
    frame.data = buffer;
    frame.width = w;
    frame.height = h;
    frame.horizontalStride = w;
    frame.verticalStride = h;
    frame.format = media::PixelFormat::NV12;
    CHECK(frame.isValid());
    CHECK(frame.data->size() == expected);
}

} // namespace

int main()
{
    testDefaultFramePacketIsInvalid();
    testValidFramePacketReportsValid();
    testSharedOwnershipKeepsBufferAliveAfterProducerRelease();
    testFramePacketIsImmutableThroughSharedConst();
    testNv12BufferSizeCalculation();

    if (g_failures == 0) {
        std::puts("frame_packet tests passed");
        return 0;
    }
    std::printf("frame_packet tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
