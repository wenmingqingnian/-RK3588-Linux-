#ifndef V4L2_CAPTURE_UTILS_H
#define V4L2_CAPTURE_UTILS_H

#include <cstdint>
#include <string>

#if defined(__linux__) || defined(linux)
#include <linux/videodev2.h>
#else
constexpr uint32_t v4l2_fourcc_fallback(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(a) |
           (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) |
           (static_cast<uint32_t>(d) << 24);
}

#define V4L2_PIX_FMT_MJPEG v4l2_fourcc_fallback('M', 'J', 'P', 'G')
#define V4L2_PIX_FMT_NV12  v4l2_fourcc_fallback('N', 'V', '1', '2')
#define V4L2_PIX_FMT_NV21  v4l2_fourcc_fallback('N', 'V', '2', '1')
#define V4L2_PIX_FMT_NV16  v4l2_fourcc_fallback('N', 'V', '1', '6')
#define V4L2_PIX_FMT_UYVY  v4l2_fourcc_fallback('U', 'Y', 'V', 'Y')
#define V4L2_PIX_FMT_YUYV  v4l2_fourcc_fallback('Y', 'U', 'Y', 'V')

#define V4L2_CAP_VIDEO_CAPTURE        0x00000001
#define V4L2_CAP_VIDEO_CAPTURE_MPLANE 0x00001000
#define V4L2_CAP_STREAMING            0x04000000
#define V4L2_CAP_DEVICE_CAPS          0x80000000

#define V4L2_BUF_TYPE_VIDEO_CAPTURE        1
#define V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE 9
#endif

namespace vehiclecamera {

struct V4L2CaptureSelection {
    uint32_t bufferType = 0;
    bool multiplanar = false;
};

uint32_t cameraFormatToV4L2Fourcc(const std::string &format,
                                  uint32_t fallback,
                                  bool *recognized = nullptr);

bool selectV4L2CaptureBufferType(uint32_t capabilities,
                                 uint32_t deviceCaps,
                                 V4L2CaptureSelection *selection);

const char *v4l2BufferTypeName(uint32_t bufferType);

} // namespace vehiclecamera

#endif // V4L2_CAPTURE_UTILS_H
