#ifndef RKNN_POSTPROCESS_H
#define RKNN_POSTPROCESS_H

// Pure C++17 image-preprocessing + YOLOv5 post-processing helpers for
// Stage B static-image inference (jilu.txt 鎼?-鎼?).
//
// Design constraints (jilu.txt 鎼傞弸鑸电€憰浣圭湴):
//   * Qt-free, NPU-free 閳?unit-testable on any host.
//   * NCHW channel-major indexing (jilu.txt 鎼俆DD line 397-402):
//       channel = anchor * (5 + classCount) + component
//       index   = (channel * gridH + row) * gridW + col
//   * No copying of the old 85-element contiguous read from rknn_detector.cpp.
//   * YOLOv5 v6 decode (grid sensitivity 2.0).

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

// Forward declaration to avoid pulling thread_pool.h into this header
// (keeps rknn_postprocess.h dependency-free for host unit tests).
namespace concurrency { class ThreadPool; }

namespace rknn_postprocess {

// RKNN-independent description of quantization metadata attached to a
// floating-point tensor. Some RKNN runtime versions report FLOAT16 tensors as
// identity AFFINE (zero-point 0, scale 1) rather than QNT_NONE.
enum class FloatTensorQuantization {
    None,
    Affine,
    Dfp,
    Unsupported
};

// Accept unquantized float tensors and the mathematically equivalent identity
// affine representation. Reject every representation that would require an
// actual dequantization transform.
bool validateFloatTensorQuantization(FloatTensorQuantization quantization,
                                     int zeroPoint, float scale,
                                     std::string &error);

// ---------- Constants ----------
constexpr int kCocoClassCount = 80;
constexpr int kYoloBoxCoords  = 5;           // cx, cy, w, h, objectness
constexpr int kChannelsPerGrid = 3;          // 3 anchors per grid
constexpr int kChannelCount    = kChannelsPerGrid * (kYoloBoxCoords + kCocoClassCount); // 255
constexpr int kMaxDetections   = 1000;       // candidate cap before NMS
constexpr int kMaxFinalDetections = 100;
constexpr float kLetterboxFill = 114.0f;

// ---------- Metadata (jilu.txt 鎼?) ----------
struct Metadata {
    std::string modelSha256;
    std::string onnxSha256;
    std::string provenance;
    std::string labelsFile;
    std::string decodeVariant;
    int  inputWidth  = 640;
    int  inputHeight = 640;
    int  inputChannels = 0;
    std::string inputLayout;
    std::string inputType;
    std::string callerInputFormat;
    std::string callerInputType;
    int callerInputPassThrough = -1;
    int  classCount  = kCocoClassCount;
    int  channelCount = 0;
    int  outputCount = 0;
    std::string outputLayout;   // "NCHW"
    std::string outputType;     // "FLOAT16"
    std::string outputRequest;  // "FLOAT32"
    std::vector<int> outputDims[3];
    std::vector<int> strides;           // {8, 16, 32}
    std::vector<int> anchorsStride8;    // {10,13,16,30,33,23}
    std::vector<int> anchorsStride16;   // {30,61,62,45,59,119}
    std::vector<int> anchorsStride32;   // {116,90,156,198,373,326}
    std::string goldenImage;
    std::string goldenImageSha256;
    std::string expectedClass;
    int  expectedClassIndex = -1;
    bool outputSigmoidBuiltin = false;  // true if model output is already sigmoided
    float gridSensitivity = 0.0f;
    int letterboxFill = -1;
};

// Parse a key=value metadata config (jilu.txt 鎼?). Returns false on parse error.
bool parseMetadata(const std::string &path, Metadata &out, std::string &err);

// Validate metadata against the actual model SHA256 and required fields
// (jilu.txt 鎼?: SHA mismatch / non-80-class / non-255-channel rejection).
bool validateMetadata(const Metadata &m, const std::string &actualModelSha256,
                      std::string &err);

// Load COCO labels (one per line). Returns false on read error.
bool loadLabels(const std::string &path, std::vector<std::string> &out, std::string &err);

// Return the exact label index for name, or -1 when name is absent.
int findClassIndexByName(const std::vector<std::string> &labels,
                         const std::string &name);

// ---------- Letterbox (jilu.txt 鎼?) ----------
struct LetterboxParams {
    float  scale   = 1.0f;
    int    padLeft   = 0;
    int    padTop    = 0;
    int    padRight  = 0;
    int    padBottom = 0;
    int    srcW = 0;
    int    srcH = 0;
    int    dstW = 0;
    int    dstH = 0;
};

// Compute letterbox params to fit (srcW x srcH) into (dstW x dstH) keeping
// aspect ratio, padding with `fill` (jilu.txt 鎼?). Handles both even and odd
// padding (jilu.txt 鎼俆DD item 6-7).
LetterboxParams computeLetterbox(int srcW, int srcH, int dstW, int dstH);

// Apply letterbox to an RGB888 source buffer (srcW*srcH*3, no stride padding)
// producing a dstW*dstH*3 buffer filled with `fill`. Returns false on bad dims.
bool applyLetterbox(const uint8_t *src, int srcW, int srcH,
                    uint8_t *dst, int dstW, int dstH,
                    const LetterboxParams &p, uint8_t fill = 114);

// Reverse letterbox: map a detection box from model coords back to original
// image coords, then clip to [0, srcW] x [0, srcH] (jilu.txt 鎼俆DD item 7-8).
// Box format: [x_center, y_center, w, h] in pixels.
struct Box { float cx, cy, w, h; };
Box reverseLetterbox(Box modelBox, const LetterboxParams &p);

// Validate an original-image center box with a scale-aware floating-point
// boundary tolerance and return a clamped normalized center box.
bool validateAndNormalizeBox(const Box &box, int imageWidth, int imageHeight,
                             Box &normalized, std::string &error);

// ---------- YOLO output layer info ----------
struct YoloLayerInfo {
    int gridW  = 0;
    int gridH  = 0;
    int stride = 0;
    int nElems = 0;
    int index  = 0;   // output index in the outputs array
};

// Identify stride from grid dimensions (jilu.txt 鎼?: 80->8, 40->16, 20->32).
// Returns 0 if grid size is unrecognized.
int strideFromGrid(int gridW, int gridH);

// Build YoloLayerInfo from grid dims + n_elems + output index.
// Validates n_elems == gridW * gridH * channelCount.
bool buildYoloLayer(int gridW, int gridH, int stride, int nElems,
                    int channelCount, int outputIndex, YoloLayerInfo &out,
                    std::string &err);

// ---------- Detection ----------
struct Detection {
    Box    box;            // center-format, original image coords
    float  confidence = 0.0f;
    int    classIndex = -1;
    std::string className;
};

// Sigmoid with NaN/Inf guard (jilu.txt 鎼俆DD item 9).
float sigmoid(float x);

// Decode one YOLOv5 v6 output layer (NCHW channel-major, jilu.txt 鎼?).
//
// data: float32 buffer of size nElems (after want_float=1 conversion).
// info: layer geometry (gridW, gridH, stride, nElems).
// anchors: 6 values {w0,h0, w1,h1, w2,h2} for this stride's 3 anchors.
// classCount: 80.
// confThreshold: minimum confidence to keep.
// outputAlreadySigmoided: true if model output is already sigmoided (values in [0,1]).
//                         false if output is raw logits (standard YOLOv5 v6 decode).
// out: appended detections (model coords, center format).
//
// Returns false on NaN/Inf or index overflow (jilu.txt 鎼? "缁備焦顒涚搾濠勬櫕鐟欙絿鐖?).
bool decodeYoloLayer(const float *data, const YoloLayerInfo &info,
                     const int anchors[6], int classCount,
                     float confThreshold, bool outputAlreadySigmoided,
                     std::vector<Detection> &out, std::string &err);

// Class-aware Non-Maximum Suppression (jilu.txt 鎼俆DD item 10-11).
// Detections of different classes never suppress each other.
// IoU has divide-by-zero protection.
std::vector<Detection> classAwareNms(const std::vector<Detection> &dets,
                                     float iouThreshold);

// Compute IoU between two boxes (center format). Returns 0 if either area is 0.
float iou(const Box &a, const Box &b);


// Non-owning view of one FLOAT32 NCHW output tensor.
struct OutputTensorView {
    const float *data = nullptr;
    std::size_t byteSize = 0;
    int gridWidth = 0;
    int gridHeight = 0;
    int outputIndex = 0;
};

// Decode all configured YOLO output layers regardless of output ordering.
// On failure, detections is empty and error describes the rejected input.
bool decodeOutputs(const std::vector<OutputTensorView> &outputs,
                   const Metadata &metadata,
                   const LetterboxParams &letterbox,
                   float confidenceThreshold,
                   float nmsThreshold,
                   std::vector<Detection> &detections,
                   std::string &error);

// Parallel variant of decodeOutputs(): the three per-stride decodeYoloLayer()
// calls are dispatched to `pool` and awaited via std::future. NMS and
// reverseLetterbox remain serial (small candidate set, no parallel benefit).
//
// Result is byte-for-byte identical to decodeOutputs() given the same inputs;
// only the scheduling differs. Provided for the production detect() path so
// the three independent CPU-bound decodes (80x80 + 40x40 + 20x20, ~2.1M float
// scans + sigmoid each) overlap on multiple cores.
//
// `pool` must outlive the call. A pool with >=3 workers is recommended so all
// three strides can run simultaneously.
bool decodeOutputsParallel(const std::vector<OutputTensorView> &outputs,
                           const Metadata &metadata,
                           const LetterboxParams &letterbox,
                           float confidenceThreshold,
                           float nmsThreshold,
                           std::vector<Detection> &detections,
                           std::string &error,
                           concurrency::ThreadPool &pool);

// ---------- Expect-class validation (jilu.txt 鎼俆DD item 14) ----------
// Returns true if at least one detection has classIndex == expectedClassIndex.
bool findExpectedClass(const std::vector<Detection> &dets, int expectedClassIndex);

} // namespace rknn_postprocess

#endif // RKNN_POSTPROCESS_H
