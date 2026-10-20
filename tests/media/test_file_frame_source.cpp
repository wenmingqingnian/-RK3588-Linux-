// Unit tests for FileFrameSource (raw NV12 file reader with loop).
// Qt-free: compiles with plain g++ under -std=c++17.
#include "file_frame_source.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
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

// Writes a raw NV12 file containing `frameCount` frames of the given size,
// with each frame's Y plane filled by a distinct sentinel byte so tests can
// tell frames apart. Returns the path written.
std::string writeTempNv12File(const std::string &path, int width, int height,
                              int frameCount)
{
    const size_t frameSize = static_cast<size_t>(width) * height * 3 / 2;
    std::vector<uint8_t> buf(frameSize * static_cast<size_t>(frameCount));
    for (int f = 0; f < frameCount; ++f) {
        uint8_t *frame = buf.data() + static_cast<size_t>(f) * frameSize;
        // Y plane: sentinel byte = 0x10 + f (so each frame is distinguishable)
        std::memset(frame, static_cast<uint8_t>(0x10 + f),
                    static_cast<size_t>(width) * height);
        // UV plane: 0x80 (neutral chroma)
        std::memset(frame + static_cast<size_t>(width) * height, 0x80,
                    static_cast<size_t>(width) * height / 2);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(buf.data()),
              static_cast<std::streamsize>(buf.size()));
    out.close();
    return path;
}

void testOpenFailsForMissingFile()
{
    media::FileFrameSource source("nonexistent_12345.nv12", 160, 120, true);
    std::string error;
    CHECK(!source.open(&error));
    CHECK(!error.empty());
}

void testOpenSucceedsForValidFile()
{
    const std::string path = "tests/media/build/sample_3frames.nv12";
    writeTempNv12File(path, 160, 120, 3);
    media::FileFrameSource source(path, 160, 120, true);
    std::string error;
    CHECK(source.open(&error));
    source.close();
}

void testReadFrameProducesValidNv12()
{
    const std::string path = "tests/media/build/sample_read.nv12";
    writeTempNv12File(path, 160, 120, 2);
    media::FileFrameSource source(path, 160, 120, true);
    std::string error;
    CHECK(source.open(&error));

    media::FramePacket frame;
    std::string readError;
    CHECK(source.readFrame(&frame, 1000, &readError) ==
          media::FrameReadResult::FrameReady);
    CHECK(frame.isValid());
    CHECK(frame.width == 160);
    CHECK(frame.height == 120);
    CHECK(frame.format == media::PixelFormat::NV12);
    CHECK(frame.data->size() == static_cast<size_t>(160) * 120 * 3 / 2);
    // First frame Y sentinel = 0x10.
    CHECK((*frame.data)[0] == 0x10);
    source.close();
}

void testSequenceAndTimestampMonotonic()
{
    const std::string path = "tests/media/build/sample_seq.nv12";
    writeTempNv12File(path, 160, 120, 3);
    media::FileFrameSource source(path, 160, 120, true);
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

void testLoopRestartsAfterEof()
{
    const std::string path = "tests/media/build/sample_loop.nv12";
    writeTempNv12File(path, 160, 120, 2);
    media::FileFrameSource source(path, 160, 120, true); // loop=true
    std::string error;
    CHECK(source.open(&error));

    media::FramePacket frame;
    std::string readError;
    // Read 5 frames from a 2-frame file: must loop without EndOfStream.
    for (int i = 0; i < 5; ++i) {
        media::FrameReadResult r = source.readFrame(&frame, 1000, &readError);
        CHECK(r == media::FrameReadResult::FrameReady);
    }
    // After looping, frame 0's sentinel 0x10 should reappear.
    CHECK((*frame.data)[0] == 0x10);
    source.close();
}

void testNoLoopReturnsEndOfStream()
{
    const std::string path = "tests/media/build/sample_noloop.nv12";
    writeTempNv12File(path, 160, 120, 2);
    media::FileFrameSource source(path, 160, 120, false); // loop=false
    std::string error;
    CHECK(source.open(&error));

    media::FramePacket frame;
    std::string readError;
    CHECK(source.readFrame(&frame, 1000, &readError) == media::FrameReadResult::FrameReady);
    CHECK(source.readFrame(&frame, 1000, &readError) == media::FrameReadResult::FrameReady);
    CHECK(source.readFrame(&frame, 1000, &readError) == media::FrameReadResult::EndOfStream);
    source.close();
}

void testRequestStopUnblocksPendingRead()
{
    const std::string path = "tests/media/build/sample_stop.nv12";
    writeTempNv12File(path, 160, 120, 1);
    media::FileFrameSource source(path, 160, 120, false);
    std::string error;
    CHECK(source.open(&error));

    media::FramePacket frame;
    std::string readError;
    // Read the one available frame.
    CHECK(source.readFrame(&frame, 1000, &readError) == media::FrameReadResult::FrameReady);
    // Next read would block waiting for a frame that never comes (no loop,
    // EOF already reached). Actually EOF returns EndOfStream immediately, so
    // to test stop-during-block we need a scenario that blocks. Use a tiny
    // file with loop=true and stop between frames — but loop never blocks.
    // Instead, verify requestStop makes a subsequent read return Stopped.
    source.requestStop();
    media::FrameReadResult r = source.readFrame(&frame, 5000, &readError);
    CHECK(r == media::FrameReadResult::Stopped);
    source.close();
}

void testInvalidDimensionsRejected()
{
    const std::string path = "tests/media/build/sample_bad.nv12";
    writeTempNv12File(path, 160, 120, 1);
    media::FileFrameSource source(path, 0, 120, true);
    std::string error;
    CHECK(!source.open(&error));
    CHECK(!error.empty());
}

void testFileSizeNotMultipleOfFrameIsFatal()
{
    // File with 1.5 frames worth of bytes -> not a multiple of frame size.
    const std::string path = "tests/media/build/sample_badsize.nv12";
    const size_t frameSize = static_cast<size_t>(160) * 120 * 3 / 2;
    std::vector<uint8_t> buf(frameSize + 10); // not a multiple
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(buf.data()),
              static_cast<std::streamsize>(buf.size()));
    out.close();

    media::FileFrameSource source(path, 160, 120, true);
    std::string error;
    CHECK(!source.open(&error));
    CHECK(!error.empty());
}

void testCloseIsIdempotentAndAllowsReopen()
{
    const std::string path = "tests/media/build/sample_reopen.nv12";
    writeTempNv12File(path, 160, 120, 1);
    media::FileFrameSource source(path, 160, 120, true);
    std::string error;
    CHECK(source.open(&error));
    source.close();
    source.close();
    std::string error2;
    CHECK(source.open(&error2));
    media::FramePacket frame;
    std::string readError;
    CHECK(source.readFrame(&frame, 1000, &readError) == media::FrameReadResult::FrameReady);
    source.close();
}

void testConfiguredFpsPacesFrames()
{
    const std::string path = "tests/media/build/sample_paced.nv12";
    writeTempNv12File(path, 160, 120, 3);
    media::FileFrameSource source(path, 160, 120, 20, true);
    std::string error;
    CHECK(source.open(&error));

    media::FramePacket frame;
    std::string readError;
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) {
        CHECK(source.readFrame(&frame, 1000, &readError) ==
              media::FrameReadResult::FrameReady);
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    // 20 fps means three delivery slots take about 150 ms. Keep a generous
    // lower bound so slow or coarse host schedulers do not make the test
    // flaky while still catching the previous unpaced implementation.
    CHECK(elapsedMs >= 100);
    CHECK(elapsedMs < 1000);
    source.close();
}

void testRequestStopInterruptsFramePacing()
{
    const std::string path = "tests/media/build/sample_paced_stop.nv12";
    writeTempNv12File(path, 160, 120, 1);
    media::FileFrameSource source(path, 160, 120, 1, true);
    std::string error;
    CHECK(source.open(&error));

    std::atomic<int> result{static_cast<int>(media::FrameReadResult::FatalError)};
    const auto started = std::chrono::steady_clock::now();
    std::thread reader([&source, &result]() {
        media::FramePacket frame;
        std::string readError;
        result.store(static_cast<int>(source.readFrame(&frame, 5000, &readError)));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    source.requestStop();
    reader.join();

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    CHECK(result.load() == static_cast<int>(media::FrameReadResult::Stopped));
    CHECK(elapsedMs < 500);
    source.close();
}

} // namespace

int main()
{
    testOpenFailsForMissingFile();
    testOpenSucceedsForValidFile();
    testReadFrameProducesValidNv12();
    testSequenceAndTimestampMonotonic();
    testLoopRestartsAfterEof();
    testNoLoopReturnsEndOfStream();
    testRequestStopUnblocksPendingRead();
    testInvalidDimensionsRejected();
    testFileSizeNotMultipleOfFrameIsFatal();
    testCloseIsIdempotentAndAllowsReopen();
    testConfiguredFpsPacesFrames();
    testRequestStopInterruptsFramePacing();

    if (g_failures == 0) {
        std::puts("file_frame_source tests passed");
        return 0;
    }
    std::printf("file_frame_source tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
