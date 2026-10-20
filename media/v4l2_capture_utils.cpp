#include "v4l2_capture_utils.h"

#include <algorithm>
#include <cctype>

namespace vehiclecamera {
namespace {

std::string normalizedFormatName(const std::string &format)
{
    std::string normalized = format;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::toupper(ch));
                   });
    return normalized;
}

uint32_t effectiveCapabilities(uint32_t capabilities, uint32_t deviceCaps)
{
    if (capabilities & V4L2_CAP_DEVICE_CAPS)
        return deviceCaps;
    return capabilities;
}

} // namespace

uint32_t cameraFormatToV4L2Fourcc(const std::string &format,
                                  uint32_t fallback,
                                  bool *recognized)
{
    const std::string normalized = normalizedFormatName(format);

    static const struct {
        const char *name;
        uint32_t fourcc;
    } formatMap[] = {
        { "NV12",  V4L2_PIX_FMT_NV12 },
        { "NV21",  V4L2_PIX_FMT_NV21 },
        { "UYVY",  V4L2_PIX_FMT_UYVY },
        { "YUYV",  V4L2_PIX_FMT_YUYV },
        { "YUY2",  V4L2_PIX_FMT_YUYV },
        { "NV16",  V4L2_PIX_FMT_NV16 },
        { "MJPEG", V4L2_PIX_FMT_MJPEG },
        { "MJPG",  V4L2_PIX_FMT_MJPEG },
    };

    for (const auto &entry : formatMap) {
        if (normalized == entry.name) {
            if (recognized)
                *recognized = true;
            return entry.fourcc;
        }
    }

    if (recognized)
        *recognized = false;
    return fallback;
}

bool selectV4L2CaptureBufferType(uint32_t capabilities,
                                 uint32_t deviceCaps,
                                 V4L2CaptureSelection *selection)
{
    if (!selection)
        return false;

    const uint32_t caps = effectiveCapabilities(capabilities, deviceCaps);
    if (!(caps & V4L2_CAP_STREAMING))
        return false;

    if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        selection->bufferType = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        selection->multiplanar = true;
        return true;
    }

    if (caps & V4L2_CAP_VIDEO_CAPTURE) {
        selection->bufferType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        selection->multiplanar = false;
        return true;
    }

    return false;
}

const char *v4l2BufferTypeName(uint32_t bufferType)
{
    switch (bufferType) {
    case V4L2_BUF_TYPE_VIDEO_CAPTURE:
        return "single-plane";
    case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
        return "multiplanar";
    default:
        return "unknown";
    }
}

} // namespace vehiclecamera
