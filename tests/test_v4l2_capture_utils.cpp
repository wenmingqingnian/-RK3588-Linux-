#include "v4l2_capture_utils.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

bool require(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

bool testUsbCameraFormatsAreRecognized()
{
    bool ok = true;
    bool recognized = false;

    ok &= require(vehiclecamera::cameraFormatToV4L2Fourcc("MJPEG", V4L2_PIX_FMT_NV12, &recognized) ==
                      V4L2_PIX_FMT_MJPEG,
                  "MJPEG should map to V4L2_PIX_FMT_MJPEG");
    ok &= require(recognized, "MJPEG should be marked recognized");

    recognized = false;
    ok &= require(vehiclecamera::cameraFormatToV4L2Fourcc("MJPG", V4L2_PIX_FMT_NV12, &recognized) ==
                      V4L2_PIX_FMT_MJPEG,
                  "MJPG alias should map to V4L2_PIX_FMT_MJPEG");
    ok &= require(recognized, "MJPG should be marked recognized");

    recognized = false;
    ok &= require(vehiclecamera::cameraFormatToV4L2Fourcc("YUYV", V4L2_PIX_FMT_NV12, &recognized) ==
                      V4L2_PIX_FMT_YUYV,
                  "YUYV should map to V4L2_PIX_FMT_YUYV");
    ok &= require(recognized, "YUYV should be marked recognized");

    recognized = true;
    ok &= require(vehiclecamera::cameraFormatToV4L2Fourcc("BAD_FORMAT", V4L2_PIX_FMT_NV12, &recognized) ==
                      V4L2_PIX_FMT_NV12,
                  "unknown formats should return fallback");
    ok &= require(!recognized, "unknown formats should not be marked recognized");

    return ok;
}

bool testUsbCameraSelectsSinglePlaneCapture()
{
    vehiclecamera::V4L2CaptureSelection selection{};
    const uint32_t caps = V4L2_CAP_DEVICE_CAPS | V4L2_CAP_STREAMING;
    const uint32_t deviceCaps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;

    bool ok = vehiclecamera::selectV4L2CaptureBufferType(caps, deviceCaps, &selection);
    ok &= require(selection.bufferType == V4L2_BUF_TYPE_VIDEO_CAPTURE,
                  "UVC Video Capture devices should use single-plane buffers");
    ok &= require(!selection.multiplanar,
                  "UVC Video Capture devices should not be marked multiplanar");
    return ok;
}

bool testRkispSelectsMultiplanarCapture()
{
    vehiclecamera::V4L2CaptureSelection selection{};
    const uint32_t caps = V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING;

    bool ok = vehiclecamera::selectV4L2CaptureBufferType(caps, 0, &selection);
    ok &= require(selection.bufferType == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                  "RK ISP capture devices should use multiplanar buffers");
    ok &= require(selection.multiplanar,
                  "RK ISP capture devices should be marked multiplanar");
    return ok;
}

bool testMetadataOnlyNodeIsRejected()
{
    vehiclecamera::V4L2CaptureSelection selection{};
    const uint32_t caps = V4L2_CAP_DEVICE_CAPS | V4L2_CAP_STREAMING;
    const uint32_t deviceCaps = V4L2_CAP_STREAMING;

    return require(!vehiclecamera::selectV4L2CaptureBufferType(caps, deviceCaps, &selection),
                   "metadata-only nodes should not be accepted as video capture");
}

} // namespace

int main()
{
    bool ok = true;
    ok &= testUsbCameraFormatsAreRecognized();
    ok &= testUsbCameraSelectsSinglePlaneCapture();
    ok &= testRkispSelectsMultiplanarCapture();
    ok &= testMetadataOnlyNodeIsRejected();

    if (!ok)
        return 1;

    std::cout << "v4l2 capture utility tests passed\n";
    return 0;
}
