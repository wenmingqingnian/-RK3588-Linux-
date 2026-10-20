#ifndef RKNN_DETECTOR_H
#define RKNN_DETECTOR_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

// Rockchip RKNN NPU object detection (YOLOv5).
//
// Uses dlopen to load librknnrt.so at runtime, so the application
// compiles without RKNN SDK headers. On systems without RKNN the
// detect() method returns an empty result and callers should skip
// inference.
//
// Thread safety: not thread-safe. Each thread should use its own instance.

namespace rknn {

// Single detected object.
//
// Coordinate semantics (jilu.txt §五.8 — unified to center-point):
//   x, y : CENTER point of the box, normalized to [0..1] relative to the
//          original image (NOT the model input).
//   w, h : width / height, normalized to [0..1] relative to the original image.
//   The previous "top-left x/y" comment was inconsistent with drawDetections()
//   and the YOLOv5 decoder; both paths have always treated x/y as center.
struct Detection {
    float x;          // CENTER x (normalized 0..1, original-image space)
    float y;          // CENTER y (normalized 0..1, original-image space)
    float w;          // Width  (normalized 0..1, original-image space)
    float h;          // Height (normalized 0..1, original-image space)
    float confidence; // Detection confidence [0..1]
    int   classId;    // Class index
};

// Detector configuration.
struct DetectorConfig {
    bool enabled = false;         // Whether RKNN detection is enabled.
    std::string modelPath;        // Path to .rknn model file
    std::string metadataPath = "config/yolov5s_metadata.conf";
    int modelWidth = 640;         // Model input width (e.g. 640)
    int modelHeight = 640;        // Model input height (e.g. 640)
    float confThreshold = 0.45f;  // Confidence threshold
    float nmsThreshold = 0.50f;   // NMS IoU threshold
};

// Load detector configuration from file (key=value format).
// Supports empty lines and # comments.
// If file is missing, returns default config with enabled=false.
// Sets *loaded=true if file was read successfully.
// Sets *errorMessage if there were issues.
DetectorConfig loadDetectorConfigFile(const std::string &path,
                                      bool *loaded,
                                      std::string *errorMessage);

// Initialize the RKNN detector (loads librknnrt.so via dlopen).
// Must be called once before using detect(). Safe to call multiple times.
// Returns true if RKNN is available and model loaded successfully.
bool initDetector(const DetectorConfig &config);

// Release RKNN detector resources. Safe to call even if init failed.
void deinitDetector();

// Run YOLOv5 inference on an RGB32 frame.
//
//   rgbData   : input RGB32 frame (width * height * 4 bytes)
//   imgWidth  : actual image width
//   imgHeight : actual image height
//   results   : output detection results (filled on success)
//
// Returns true if inference completed (results may be empty if no objects found).
// Returns false on error (model not loaded, inference failure, etc.).
bool detect(const uint8_t *rgbData, int imgWidth, int imgHeight,
            std::vector<Detection> &results);

// Draw detection bounding boxes and labels onto an RGB32 image (in-place).
//
//   rgbData   : RGB32 image buffer (modified in-place)
//   imgWidth  : image width
//   imgHeight : image height
//   detections: detection results from detect()
//   classNames: class name strings (e.g. COCO 80 classes)
void drawDetections(uint8_t *rgbData, int imgWidth, int imgHeight,
                    const std::vector<Detection> &detections,
                    const std::vector<std::string> &classNames);

// Get default COCO 80 class names.
const std::vector<std::string> &cocoClassNames();

} // namespace rknn

#endif // RKNN_DETECTOR_H
