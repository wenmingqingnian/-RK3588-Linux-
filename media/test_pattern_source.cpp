#include "test_pattern_source.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace media {

namespace {

inline uint64_t nowUs()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch())
            .count());
}

// BT.601 YUV values for the 8 SMPTE color bars.
struct YuvColor { uint8_t y; uint8_t u; uint8_t v; };
const YuvColor kBars[] = {
    { 235, 128, 128 }, // white
    { 210,  16, 146 }, // yellow
    { 170, 166,  16 }, // cyan
    { 145,  54,  34 }, // green
    { 107, 202, 222 }, // magenta
    {  82,  90, 240 }, // red
    {  41, 240, 110 }, // blue
    {  16, 128, 128 }, // black
};

} // namespace

TestPatternSource::TestPatternSource(int width, int height, int fps)
    : m_width(width)
    , m_height(height)
    , m_fps(fps)
    , m_frameIntervalUs(fps > 0 ? 1000000 / fps : 40000)
{
}

TestPatternSource::TestPatternSource(int width, int height, int fps,
                                     std::shared_ptr<pool::FrameBufferPool> pool)
    : m_width(width)
    , m_height(height)
    , m_fps(fps)
    , m_frameIntervalUs(fps > 0 ? 1000000 / fps : 40000)
    , m_pool(std::move(pool))
{
}

TestPatternSource::~TestPatternSource()
{
    close();
}

bool TestPatternSource::open(std::string *errorMessage)
{
    if (m_width <= 0 || m_height <= 0) {
        if (errorMessage) *errorMessage = "test_pattern: invalid dimensions";
        return false;
    }
    if ((m_width % 2) != 0 || (m_height % 2) != 0) {
        if (errorMessage) *errorMessage = "test_pattern: NV12 requires even dimensions";
        return false;
    }
    if (m_fps < 1 || m_fps > 120) {
        if (errorMessage) *errorMessage = "test_pattern: fps must be in [1..120]";
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_stopRequested = false;
    m_sequence = 0;
    m_openTimeUs = nowUs();
    m_open = true;
    return true;
}

FrameReadResult TestPatternSource::readFrame(FramePacket *frame,
                                             int timeoutMs,
                                             std::string *errorMessage)
{
    if (!m_open) {
        if (errorMessage) *errorMessage = "test_pattern: not opened";
        return FrameReadResult::FatalError;
    }

    // Pace output to the configured fps. Compute how long to wait until the
    // next frame slot, then wait on the condition variable so requestStop()
    // can interrupt us promptly.
    const uint64_t now = nowUs();
    const uint64_t elapsed = now - m_openTimeUs;
    const uint64_t targetTime = (m_sequence + 1) * static_cast<uint64_t>(m_frameIntervalUs);
    int64_t waitUs = static_cast<int64_t>(targetTime) - static_cast<int64_t>(elapsed);
    if (waitUs < 0) waitUs = 0;
    // Cap the self-imposed wait by the caller's timeout.
    const int64_t timeoutUs = static_cast<int64_t>(timeoutMs) * 1000;
    if (waitUs > timeoutUs) waitUs = timeoutUs;

    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_cv.wait_for(lock, std::chrono::microseconds(waitUs),
                          [this]() { return m_stopRequested.load(); })) {
            // Predicate true => stop requested.
            return FrameReadResult::Stopped;
        }
        if (m_stopRequested) {
            return FrameReadResult::Stopped;
        }
    }

    // Render the frame.
    const size_t bufSize = pixelFormatBufferSize(PixelFormat::NV12, m_width, m_height);
    std::shared_ptr<std::vector<uint8_t>> buffer;
    if (m_pool && m_pool->statistics().capacityBytes == bufSize) {
        buffer = m_pool->acquire();
    } else {
        buffer = std::make_shared<std::vector<uint8_t>>(bufSize);
    }
    renderNv12Frame(buffer->data(), m_width, m_height, m_sequence);

    frame->data = buffer;  // shared_ptr<vector> → shared_ptr<const vector>
    frame->dmabufFd = -1;
    frame->width = m_width;
    frame->height = m_height;
    frame->horizontalStride = m_width;
    frame->verticalStride = m_height;
    frame->format = PixelFormat::NV12;
    frame->sequence = m_sequence;
    frame->timestampUs = nowUs();

    ++m_sequence;
    return FrameReadResult::FrameReady;
}

void TestPatternSource::requestStop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopRequested = true;
    }
    m_cv.notify_all();
}

void TestPatternSource::close()
{
    requestStop();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_open = false;
}

void TestPatternSource::renderNv12Frame(uint8_t *dst, int width, int height,
                                        uint64_t sequence)
{
    // Y plane.
    uint8_t *yPlane = dst;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int bar = std::min((x * 8) / width, 7);
            yPlane[y * width + x] = kBars[bar].y;
        }
    }

    // Moving white square (overlays a bright block to show motion).
    const int sqW = std::min(80, width);
    const int sqH = std::min(80, height);
    const int horizontalTravel = width - sqW;
    int sx = horizontalTravel > 0
        ? static_cast<int>((sequence * 6) %
                           static_cast<uint64_t>(horizontalTravel + 1))
        : 0;
    int sy = height / 2 - sqH / 2 +
             static_cast<int>(60.0 * std::sin(static_cast<double>(sequence) * 0.1));
    if (sy < 0) sy = 0;
    if (sy + sqH > height) sy = height - sqH;
    for (int y = sy; y < sy + sqH; ++y) {
        for (int x = sx; x < sx + sqW; ++x) {
            yPlane[y * width + x] = 235; // white
        }
    }

    // Interleaved UV plane (NV12: pairs of (U,V) shared by 2x2 luma block).
    uint8_t *uvPlane = dst + static_cast<size_t>(width) * height;
    for (int y = 0; y < height / 2; ++y) {
        for (int x = 0; x < width / 2; ++x) {
            const int bar = std::min((x * 16) / width, 7);
            uvPlane[(y * (width / 2) + x) * 2 + 0] = kBars[bar].u;
            uvPlane[(y * (width / 2) + x) * 2 + 1] = kBars[bar].v;
        }
    }
}

} // namespace media
