#ifndef MEDIA_TEST_PATTERN_SOURCE_H
#define MEDIA_TEST_PATTERN_SOURCE_H

#include "frame_source.h"
#include "frame_buffer_pool.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace media {

// Generates a fixed-size NV12 test pattern (8 color bars + a moving white
// square) at a configurable frame rate. Used to drive the full pipeline
// (RGA → GUI, RKNN, MPP encode → RTSP) without camera hardware.
//
// Thread-safety: open/readFrame/requestStop/close may be called from any
// thread, but readFrame is intended to be called from a single source thread.
// requestStop() is safe to call concurrently with a pending readFrame().
//
// Optional buffer pool: see FileFrameSource. capacityBytes must equal
// width*height*3/2 to take effect.
class TestPatternSource : public IFrameSource {
public:
    // width/height must be positive even values (NV12 requires even dims).
    // fps must be in [1..120].
    TestPatternSource(int width, int height, int fps);
    TestPatternSource(int width, int height, int fps,
                      std::shared_ptr<pool::FrameBufferPool> pool);

    ~TestPatternSource() override;

    bool open(std::string *errorMessage) override;
    FrameReadResult readFrame(FramePacket *frame,
                              int timeoutMs,
                              std::string *errorMessage) override;
    void requestStop() override;
    void close() override;

private:
    void renderNv12Frame(uint8_t *dst, int width, int height,
                         uint64_t sequence);

    int m_width;
    int m_height;
    int m_fps;
    int m_frameIntervalUs;   // 1_000_000 / fps

    std::shared_ptr<pool::FrameBufferPool> m_pool;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_open{false};
    std::atomic<bool> m_stopRequested{false};
    uint64_t m_sequence = 0;
    uint64_t m_openTimeUs = 0;
};

} // namespace media

#endif // MEDIA_TEST_PATTERN_SOURCE_H
