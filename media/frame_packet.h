#ifndef MEDIA_FRAME_PACKET_H
#define MEDIA_FRAME_PACKET_H

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

// Frame memory ownership and dispatch contract for the media pipeline.
//
// Design rules (task §四):
//   1. The producer (frame source) creates the frame buffer.
//   2. The buffer is held by std::shared_ptr<const std::vector<uint8_t>> so
//      multiple consumers (GUI/RKNN/RTSP) can share the SAME read-only frame
//      without copying, and the buffer is released when the last consumer
//      drops its reference.
//   3. The buffer contents are immutable (shared_ptr<const ...>), which makes
//      cross-thread sharing safe without per-consumer locks on the payload.
//   4. dmabufFd / strides are reserved for a future DMA-BUF zero-copy path;
//      in this task the payload is always backed by the shared vector.
//   5. Stopping the pipeline waits for in-flight frames to be released by
//      draining the broker queues and joining the consumer threads (see
//      FrameBroker and MediaPipeline).

namespace media {

enum class PixelFormat {
    Unknown,
    NV12,   // Y plane (W*H) + interleaved UV plane (W*H/2), 4:2:0
    RGB32,  // packed BGRA/XRGB, 4 bytes per pixel
};

inline const char *pixelFormatToString(PixelFormat format)
{
    switch (format) {
    case PixelFormat::NV12:   return "NV12";
    case PixelFormat::RGB32:  return "RGB32";
    case PixelFormat::Unknown: return "Unknown";
    }
    return "Unknown";
}

// Number of bytes required to store one frame of the given format/dimensions.
inline size_t pixelFormatBufferSize(PixelFormat format, int width, int height)
{
    switch (format) {
    case PixelFormat::NV12:
        return static_cast<size_t>(width) * static_cast<size_t>(height) * 3 / 2;
    case PixelFormat::RGB32:
        return static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    case PixelFormat::Unknown:
        return 0;
    }
    return 0;
}

struct FramePacket {
    // Shared read-only payload. nullptr means the frame is empty/invalid.
    // Producer creates via std::make_shared<std::vector<uint8_t>>(...);
    // consumers only read through this const handle.
    std::shared_ptr<const std::vector<uint8_t>> data;

    // Reserved for future DMA-BUF zero-copy (task §十五: do not claim
    // zero-copy is done without a real camera). Always -1 in this task.
    int dmabufFd = -1;

    int width = 0;
    int height = 0;
    int horizontalStride = 0;   // bytes per row of the Y plane (>= width)
    int verticalStride = 0;     // rows in the Y plane including padding (>= height)

    PixelFormat format = PixelFormat::Unknown;

    // Monotonic timestamp in microseconds (wall clock from source).
    uint64_t timestampUs = 0;
    // Monotonically increasing sequence number per source.
    uint64_t sequence = 0;

    bool isValid() const
    {
        return data != nullptr && width > 0 && height > 0 &&
               format != PixelFormat::Unknown;
    }
};

} // namespace media

#endif // MEDIA_FRAME_PACKET_H
