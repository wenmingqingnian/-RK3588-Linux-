#ifndef VEHICLECAMERA_CAMERACONFIG_H
#define VEHICLECAMERA_CAMERACONFIG_H

#include <string>
#include <stdexcept>

namespace vehiclecamera {

struct CameraConfig {
    bool enabled = false;
    std::string device;         // e.g. /dev/video33
    int width = 800;
    int height = 600;
    std::string format;         // e.g. NV12
    int bufferCount = 4;
    int fps = 30;

    // Media source selection (task: software end-to-end loop without camera).
    // sourceType: "v4l2" (default, backward compat), "test_pattern", "file".
    std::string sourceType = "v4l2";
    std::string filePath;          // for sourceType="file"
    bool fileLoop = true;          // loop file after EOF
    int queueCapacity = 3;         // per-lane broker queue depth
    int startTimeoutMs = 5000;     // V4L2 CaptureThread init timeout
    int readTimeoutMs = 500;       // media-source per-frame read timeout
    int stopTimeoutMs = 3000;      // V4L2 CaptureThread shutdown timeout
};

class CameraConfigError : public std::runtime_error {
public:
    explicit CameraConfigError(const std::string &message);
};

// Parse camera configuration from text (key=value format).
// Supports empty lines and # comments.
// Throws CameraConfigError on invalid config.
CameraConfig parseCameraConfigText(const std::string &configText);

// Load camera configuration from file.
// If file is missing, returns default config with enabled=false.
// Sets *loaded=true if file was read successfully.
// Sets *errorMessage if there were issues.
CameraConfig loadCameraConfigFile(const std::string &path,
                                  bool *loaded,
                                  std::string *errorMessage);

} // namespace vehiclecamera

#endif // VEHICLECAMERA_CAMERACONFIG_H
