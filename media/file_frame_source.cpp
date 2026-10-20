#include "file_frame_source.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
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

// Returns file size in bytes, or static_cast<size_t>(-1) on error.
size_t fileSize(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return static_cast<size_t>(-1);
    return static_cast<size_t>(st.st_size);
}

} // namespace

FileFrameSource::FileFrameSource(const std::string &path,
                                 int width,
                                 int height,
                                 bool loop)
    : FileFrameSource(path, width, height, 30, loop)
{
}

FileFrameSource::FileFrameSource(const std::string &path,
                                 int width,
                                 int height,
                                 int fps,
                                 bool loop)
    : m_path(path)
    , m_width(width)
    , m_height(height)
    , m_fps(fps)
    , m_frameIntervalUs(fps > 0 ? 1000000 / fps : 0)
    , m_loop(loop)
{
}

FileFrameSource::FileFrameSource(const std::string &path,
                                 int width,
                                 int height,
                                 int fps,
                                 bool loop,
                                 std::shared_ptr<pool::FrameBufferPool> pool)
    : m_path(path)
    , m_width(width)
    , m_height(height)
    , m_fps(fps)
    , m_frameIntervalUs(fps > 0 ? 1000000 / fps : 0)
    , m_loop(loop)
    , m_pool(std::move(pool))
{
}

FileFrameSource::~FileFrameSource()
{
    close();
}

bool FileFrameSource::open(std::string *errorMessage)
{
    // Validate dimensions before touching the filesystem so unit tests can
    // exercise rejection paths without creating files.
    if (m_width <= 0 || m_height <= 0) {
        if (errorMessage) *errorMessage = "file_source: invalid dimensions";
        return false;
    }
    if ((m_width % 2) != 0 || (m_height % 2) != 0) {
        if (errorMessage) *errorMessage = "file_source: NV12 requires even dimensions";
        return false;
    }
    if (m_fps < 1 || m_fps > 120) {
        if (errorMessage) *errorMessage = "file_source: fps must be in [1..120]";
        return false;
    }

    const size_t frameSize =
        pixelFormatBufferSize(PixelFormat::NV12, m_width, m_height);
    if (frameSize == 0) {
        if (errorMessage) *errorMessage = "file_source: zero frame size";
        return false;
    }

    const size_t sz = fileSize(m_path);
    if (sz == static_cast<size_t>(-1)) {
        if (errorMessage)
            *errorMessage = "file_source: file not found: " + m_path;
        return false;
    }
    if (sz == 0) {
        if (errorMessage) *errorMessage = "file_source: empty file";
        return false;
    }
    if ((sz % frameSize) != 0) {
        if (errorMessage) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "file_source: file size %zu not a multiple of frame size %zu",
                          sz, frameSize);
            *errorMessage = buf;
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.is_open())
        m_file.close();

    m_file.open(m_path, std::ios::binary);
    if (!m_file.is_open()) {
        if (errorMessage)
            *errorMessage = "file_source: failed to open file: " + m_path;
        return false;
    }

    m_frameSize = frameSize;
    m_stopRequested = false;
    m_sequence = 0;
    m_openTimeUs = nowUs();
    m_open = true;
    return true;
}

FrameReadResult FileFrameSource::readFrame(FramePacket *frame,
                                           int timeoutMs,
                                           std::string *errorMessage)
{
    if (!m_open) {
        if (errorMessage) *errorMessage = "file_source: not opened";
        return FrameReadResult::FatalError;
    }
    if (m_stopRequested) {
        return FrameReadResult::Stopped;
    }
    // Deliver frames according to the configured media rate instead of
    // flooding the broker at storage speed. The condition-variable wait is
    // interruptible so stop() does not have to wait for the next frame slot.
    const uint64_t now = nowUs();
    const uint64_t elapsed = now - m_openTimeUs;
    const uint64_t targetTime =
        (m_sequence + 1) * static_cast<uint64_t>(m_frameIntervalUs);
    int64_t waitUs = static_cast<int64_t>(targetTime) -
                     static_cast<int64_t>(elapsed);
    if (waitUs > 0) {
        const int64_t timeoutUs =
            timeoutMs > 0 ? static_cast<int64_t>(timeoutMs) * 1000 : 0;
        const int64_t actualWaitUs =
            timeoutUs > 0 ? std::min(waitUs, timeoutUs) : 0;

        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_cv.wait_for(lock, std::chrono::microseconds(actualWaitUs),
                          [this]() { return m_stopRequested.load(); })) {
            return FrameReadResult::Stopped;
        }
        if (m_stopRequested) {
            return FrameReadResult::Stopped;
        }
        if (actualWaitUs < waitUs) {
            return FrameReadResult::Timeout;
        }
    }

    // Read one frame's worth of bytes. If EOF is hit, either loop back to
    // byte 0 (loop=true) or return EndOfStream (loop=false). The file read
    // itself is non-blocking on a local file, so we do not need to pump the
    // condition variable here; requestStop() is observed via the atomic flag
    // on the next call.
    // 帧缓冲分配：若提供内存池且容量匹配，从池中借出；否则回退到 heap。
    // 池借出的 shared_ptr 引用归零时由 FrameBufferPool::Releaser 自动归还。
    std::shared_ptr<std::vector<uint8_t>> shared;
    if (m_pool && m_pool->statistics().capacityBytes == m_frameSize) {
        shared = m_pool->acquire();
    } else {
        // 无池或容量不匹配：回退到 heap 分配。
        // 容量不匹配属配置错误，调用方可通过 statistics().currentInUse
        // 持续为 0 观察到该情况。
        shared = std::make_shared<std::vector<uint8_t>>(m_frameSize);
    }

    m_file.read(reinterpret_cast<char *>(shared->data()),
                static_cast<std::streamsize>(m_frameSize));

    if (m_file.gcount() != static_cast<std::streamsize>(m_frameSize)) {
        // Partial or no data => EOF.
        if (m_stopRequested) {
            return FrameReadResult::Stopped;
        }
        if (m_loop) {
            m_file.clear();
            m_file.seekg(0, std::ios::beg);
            m_file.read(reinterpret_cast<char *>(shared->data()),
                        static_cast<std::streamsize>(m_frameSize));
            if (m_file.gcount() != static_cast<std::streamsize>(m_frameSize)) {
                if (errorMessage)
                    *errorMessage = "file_source: re-read after seek failed";
                return FrameReadResult::FatalError;
            }
        } else {
            return FrameReadResult::EndOfStream;
        }
    }

    if (m_stopRequested) {
        return FrameReadResult::Stopped;
    }

    frame->data = shared;  // shared_ptr<vector> → shared_ptr<const vector>
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

void FileFrameSource::requestStop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopRequested = true;
    }
    m_cv.notify_all();
}

void FileFrameSource::close()
{
    requestStop();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.is_open())
        m_file.close();
    m_open = false;
}

} // namespace media
