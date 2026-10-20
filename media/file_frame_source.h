#ifndef MEDIA_FILE_FRAME_SOURCE_H
#define MEDIA_FILE_FRAME_SOURCE_H

#include "frame_source.h"

#include "frame_buffer_pool.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

namespace media {

// Reads raw NV12 frames from a file. The file must contain an integer number
// of frames of size width*height*3/2. Frames are paced at the configured FPS.
// When loop is true, the source restarts from byte 0 after EOF; otherwise
// readFrame returns EndOfStream.
//
// Thread-safety: open/readFrame/requestStop/close may be called from any
// thread, but readFrame is intended to be called from a single source thread.
// requestStop() is safe to call concurrently with a pending readFrame().
//
// Optional buffer pool: if a FrameBufferPool is supplied at construction and
// its capacityBytes matches the NV12 frame size (width*height*3/2), each
// readFrame() acquires a block from the pool instead of heap-allocating a
// new std::vector. When the pool is null the source falls back to the
// original make_shared<vector<uint8_t>> path (zero behavior change).
class FileFrameSource : public IFrameSource {
public:
    // width/height must be positive even values (NV12 requires even dims).
    FileFrameSource(const std::string &path, int width, int height, bool loop);
    FileFrameSource(const std::string &path, int width, int height, int fps,
                    bool loop);
    // Pool-enabled constructor. capacityBytes of the pool must equal
    // width*height*3/2; if it does not match, the pool is ignored and a
    // one-time WARN is logged via LogManager.
    FileFrameSource(const std::string &path, int width, int height, int fps,
                    bool loop, std::shared_ptr<pool::FrameBufferPool> pool);

    ~FileFrameSource() override;

    bool open(std::string *errorMessage) override;
    FrameReadResult readFrame(FramePacket *frame,
                              int timeoutMs,
                              std::string *errorMessage) override;
    void requestStop() override;
    void close() override;

private:
    std::string m_path;
    int m_width;
    int m_height;
    int m_fps;
    int64_t m_frameIntervalUs;
    bool m_loop;

    std::shared_ptr<pool::FrameBufferPool> m_pool;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_open{false};
    std::atomic<bool> m_stopRequested{false};

    std::ifstream m_file;
    size_t m_frameSize = 0;
    uint64_t m_sequence = 0;
    uint64_t m_openTimeUs = 0;
};

} // namespace media

#endif // MEDIA_FILE_FRAME_SOURCE_H
