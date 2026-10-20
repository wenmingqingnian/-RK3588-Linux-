#include "rknn_detector.h"
#include "rknn_postprocess.h"
#include "logmanager.h"
#include "thread_pool.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QIODevice>
#include <QString>

#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <vector>
#include <string>

// ============================================================================
// Stage A refactor (jilu.txt §二, §四, §六):
//
//   * All hand-written RKNN ABI structs (rknn_context / rknn_input /
//     rknn_output / rknn_tensor_attr / rknn_input_output_num / rknn_query_cmd
//     / rknn_tensor_type / rknn_tensor_format) have been DELETED (§P0-1).
//
//   * The OFFICIAL <rknn_api.h> is used via "rknn_runtime.h". Function pointer
//     types are derived with decltype(&::fn) inside rknn_runtime.h (§P0-1,§四.3).
//
//   * rknn_init uses the 5-parameter signature (extend=nullptr) — handled
//     inside rknn_runtime::Runtime::initModel() (§P0-2).
//
//   * Explicit rknn_run() is now called between inputs_set and outputs_get
//     (§P0-3). The previous assumption that outputs_get runs the model is
//     removed.
//
//   * rknn_outputs_get passes nullptr for rknn_output_extend* (§P0-4) —
//     handled inside Runtime::outputsGet().
//
//   * rknn_input uses the official field layout: index/buf/size/pass_through/
//     type/fmt. The non-existent `buf_size` field is gone (§P0-5).
//
//   * rknn_output uses the official layout: want_float/is_prealloc/index/buf/
//     size (§P0-6).
//
//   * rknn_tensor_attr comes from the official header; field order, qnt_type,
//     and h_stride type are now correct by construction (§P0-7).
//
//   * Input buffer is allocated as W*H*3 (RGB888), not W*H*4 (RGB32). The
//     actual C/H/W are derived from input_attrs[0] at init time (§P0-8).
//
//   * Qt RGB32 → RGB888 conversion is performed explicitly in detect().
//     We never feed 4-byte pixels to the model (§P0-9).
//
//   * Output post-processing validates output_attrs (n_elems, dims, type,
//     qnt_type) before decoding. The native contract is three FLOAT16 NCHW
//     outputs with NONE or identity-AFFINE quantization; mismatches fail
//     before any output buffer is decoded (§P0-10).
//
// The tracked config keeps rknn_enabled=false until the controlled live
// person-alarm trigger/clear and shutdown-silence gate passes on the board.
// ============================================================================

static const char *MODULE = "rknn";

#if __has_include(<rknn_api.h>)

#include "rknn_runtime.h"

// ============================================================================
// Internal state (per §P1-2: documented as not thread-safe; each thread
// should use its own instance. The free-function API preserved for
// Monitor.cpp compatibility wraps a single static Runtime instance. Stage C
// will replace this with an instance class + worker thread, per jilu.txt §六.)
// ============================================================================
static rknn_runtime::Runtime *s_rt = nullptr;
static bool                   s_detectorReady = false;
static rknn::DetectorConfig   s_config;
static rknn_postprocess::Metadata s_metadata;

// Decode thread pool: 3 workers for the 3 YOLOv5 output strides (8/16/32).
// Lazily initialized in initDetector(), torn down in clearDetectorState().
// Owned via unique_ptr so the ThreadPool can be destroyed (and its workers
// joined) before the static duration ends — avoiding detached-thread issues
// at process exit.
static std::unique_ptr<concurrency::ThreadPool> s_decodePool;

// Queried once at init time.
static rknn_input_output_num        s_ioNum{};
static std::vector<rknn_tensor_attr> s_inputAttrs;
static std::vector<rknn_tensor_attr> s_outputAttrs;

// Derived input geometry from input_attrs[0] (§P0-8).
static uint32_t s_inputN = 0;
static uint32_t s_inputC = 0;
static uint32_t s_inputH = 0;
static uint32_t s_inputW = 0;
static rknn_tensor_format s_inputFmt = RKNN_TENSOR_NCHW;

// Pre-allocated RGB888 input buffer (W*H*3) — never 4-byte.
static std::vector<uint8_t> s_inputBuffer;

static const char *COCO_NAMES[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck",
    "boat", "traffic light", "fire hydrant", "stop sign", "parking meter", "bench",
    "bird", "cat", "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra",
    "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove",
    "skateboard", "surfboard", "tennis racket", "bottle", "wine glass", "cup",
    "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
    "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush"
};

static const int COCO_CLASS_COUNT = 80;

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

static const char *tensorTypeStr(rknn_tensor_type t)
{
    switch (t) {
    case RKNN_TENSOR_FLOAT32: return "FLOAT32";
    case RKNN_TENSOR_FLOAT16: return "FLOAT16";
    case RKNN_TENSOR_INT8:    return "INT8";
    case RKNN_TENSOR_UINT8:   return "UINT8";
    case RKNN_TENSOR_INT16:   return "INT16";
    case RKNN_TENSOR_UINT16:  return "UINT16";
    case RKNN_TENSOR_INT32:   return "INT32";
    case RKNN_TENSOR_UINT32:  return "UINT32";
    case RKNN_TENSOR_INT64:   return "INT64";
    case RKNN_TENSOR_BOOL:    return "BOOL";
    case RKNN_TENSOR_TYPE_MAX:return "UNKNOWN";
    default:                  return "UNKNOWN";
    }
}

static const char *tensorFmtStr(rknn_tensor_format f)
{
    switch (f) {
    case RKNN_TENSOR_NCHW:     return "NCHW";
    case RKNN_TENSOR_NHWC:     return "NHWC";
    case RKNN_TENSOR_NC1HWC2:  return "NC1HWC2";
    case RKNN_TENSOR_UNDEFINED:return "UNDEFINED";
    case RKNN_TENSOR_FORMAT_MAX:return "UNKNOWN";
    default:                   return "UNKNOWN";
    }
}

static const char *tensorQntStr(rknn_tensor_qnt_type qnt)
{
    switch (qnt) {
    case RKNN_TENSOR_QNT_NONE:              return "NONE";
    case RKNN_TENSOR_QNT_DFP:               return "DFP";
    case RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC: return "AFFINE";
    case RKNN_TENSOR_QNT_MAX:               return "UNKNOWN";
    default:                                return "UNKNOWN";
    }
}

static rknn_postprocess::FloatTensorQuantization
floatTensorQuantization(rknn_tensor_qnt_type qnt)
{
    switch (qnt) {
    case RKNN_TENSOR_QNT_NONE:
        return rknn_postprocess::FloatTensorQuantization::None;
    case RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC:
        return rknn_postprocess::FloatTensorQuantization::Affine;
    case RKNN_TENSOR_QNT_DFP:
        return rknn_postprocess::FloatTensorQuantization::Dfp;
    case RKNN_TENSOR_QNT_MAX:
        return rknn_postprocess::FloatTensorQuantization::Unsupported;
    default:
        return rknn_postprocess::FloatTensorQuantization::Unsupported;
    }
}

// Derive N/C/H/W from a tensor attribute (jilu.txt §四.10, §P0-8).
static bool deriveInputDims(const rknn_tensor_attr &a,
                            uint32_t &outN, uint32_t &outC,
                            uint32_t &outH, uint32_t &outW,
                            std::string &err)
{
    if (a.n_dims != 4) {
        err = "expected 4-D input tensor, got n_dims=" + std::to_string(a.n_dims);
        return false;
    }
    if (a.fmt == RKNN_TENSOR_NHWC) {
        outN = a.dims[0]; outH = a.dims[1]; outW = a.dims[2]; outC = a.dims[3];
    } else if (a.fmt == RKNN_TENSOR_NCHW) {
        outN = a.dims[0]; outC = a.dims[1]; outH = a.dims[2]; outW = a.dims[3];
    } else {
        err = std::string("unsupported input fmt: ") + tensorFmtStr(a.fmt);
        return false;
    }
    if (outN != 1) {
        err = "expected batch=1, got N=" + std::to_string(outN);
        return false;
    }
    if (outC != 3) {
        err = "expected channels=3 (RGB), got C=" + std::to_string(outC);
        return false;
    }
    if (outH == 0 || outW == 0) {
        err = "zero H or W in input tensor";
        return false;
    }
    return true;
}

// Safe multiply with overflow check.
static bool safeMul3(uint32_t a, uint32_t b, uint32_t c, size_t &out)
{
    uint64_t r = static_cast<uint64_t>(a) * b;
    if (r > SIZE_MAX) return false;
    r *= c;
    if (r > SIZE_MAX) return false;
    out = static_cast<size_t>(r);
    return true;
}

// Letterbox resize RGB888 src → RGB888 dst with gray (114) padding.
// (jilu.txt §五.3 — Stage B path. Kept for Stage A compilation; not exercised
// while rknn_enabled=false.)
// Convert Qt RGB32 (B,G,R,X on little-endian ARM) to RGB888 (R,G,B).
// (jilu.txt §P0-9 — explicit conversion, never feed 4-byte pixels to model.)
static void convertRGB32toRGB888(const uint8_t *rgb32, int W, int H,
                                 uint8_t *rgb888Out)
{
    for (int i = 0; i < W * H; ++i) {
        // QImage::Format_RGB32 packs as 0xffRRGGBB on big-endian, but on
        // little-endian ARM the byte order in memory is B, G, R, X.
        uint8_t b = rgb32[i * 4 + 0];
        uint8_t g = rgb32[i * 4 + 1];
        uint8_t r = rgb32[i * 4 + 2];
        rgb888Out[i * 3 + 0] = r;
        rgb888Out[i * 3 + 1] = g;
        rgb888Out[i * 3 + 2] = b;
    }
}

static void clearDetectorState()
{
    if (s_rt) {
        s_rt->deinit();
        delete s_rt;
        s_rt = nullptr;
    }
    // Destroy the pool first so any in-flight decode tasks finish (or are
    // discarded) before the buffers they reference are released below.
    s_decodePool.reset();
    s_inputBuffer.clear();
    s_inputAttrs.clear();
    s_outputAttrs.clear();
    s_inputN = s_inputC = s_inputH = s_inputW = 0;
    s_inputFmt = RKNN_TENSOR_NCHW;
    s_ioNum = rknn_input_output_num{};
    s_config = rknn::DetectorConfig{};
    s_metadata = rknn_postprocess::Metadata{};
    s_detectorReady = false;
}

static bool computeFileSha256(const std::string &path, std::string &sha,
                              std::string &error)
{
    sha.clear();
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) {
        error = "cannot open model for SHA-256: " + path + ": " +
                file.errorString().toStdString();
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    constexpr qint64 kChunkSize = 1024 * 1024;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(kChunkSize);
        if (chunk.isEmpty()) {
            if (file.error() != QFileDevice::NoError) {
                error = "error reading model for SHA-256: " + path + ": " +
                        file.errorString().toStdString();
                return false;
            }
            break;
        }
        hash.addData(chunk);
    }
    if (file.error() != QFileDevice::NoError) {
        error = "error reading model for SHA-256: " + path + ": " +
                file.errorString().toStdString();
        return false;
    }
    sha = hash.result().toHex().toLower().toStdString();
    return true;
}

static int validateOutputAttr(const rknn_tensor_attr &attr, std::string &error)
{
    if (attr.n_dims != 4 || attr.fmt != RKNN_TENSOR_NCHW) {
        error = "expected four-dimensional NCHW output";
        return 0;
    }
    if (attr.dims[0] != 1 ||
        attr.dims[1] != static_cast<uint32_t>(rknn_postprocess::kChannelCount)) {
        error = "expected output batch=1 and channels=255";
        return 0;
    }
    if (attr.type != RKNN_TENSOR_FLOAT16 ||
        s_metadata.outputType != "FLOAT16") {
        error = "native output type does not match metadata FLOAT16";
        return 0;
    }
    std::string quantizationError;
    if (!rknn_postprocess::validateFloatTensorQuantization(
            floatTensorQuantization(attr.qnt_type), attr.zp, attr.scale,
            quantizationError)) {
        std::ostringstream message;
        message << "native output quantization rejected: qnt="
                << tensorQntStr(attr.qnt_type) << " zp=" << attr.zp
                << " scale=" << attr.scale << ": " << quantizationError;
        error = message.str();
        return 0;
    }

    const int gridH = static_cast<int>(attr.dims[2]);
    const int gridW = static_cast<int>(attr.dims[3]);
    const int stride = rknn_postprocess::strideFromGrid(gridW, gridH);
    const uint64_t expectedElems = static_cast<uint64_t>(gridW) * gridH *
                                   rknn_postprocess::kChannelCount;
    int metadataSlot = stride == 8 ? 0 : (stride == 16 ? 1 : (stride == 32 ? 2 : -1));
    const size_t expectedBytes = static_cast<size_t>(expectedElems) * sizeof(uint16_t);
    if (stride == 0 || metadataSlot < 0 || attr.n_elems != expectedElems ||
        attr.size != expectedBytes || s_metadata.outputDims[metadataSlot].size() != 4 ||
        attr.dims[0] != static_cast<uint32_t>(s_metadata.outputDims[metadataSlot][0]) ||
        attr.dims[1] != static_cast<uint32_t>(s_metadata.outputDims[metadataSlot][1]) ||
        attr.dims[2] != static_cast<uint32_t>(s_metadata.outputDims[metadataSlot][2]) ||
        attr.dims[3] != static_cast<uint32_t>(s_metadata.outputDims[metadataSlot][3])) {
        error = "unrecognized output grid or inconsistent element count";
        return 0;
    }
    return stride;
}

// ----------------------------------------------------------------------------
// Public API (free-function form preserved for Monitor.cpp compatibility)
// ----------------------------------------------------------------------------

bool rknn::initDetector(const DetectorConfig &config)
{
    if (s_detectorReady) return true;

    clearDetectorState();

    if (config.modelPath.empty() || config.metadataPath.empty()) {
        VLOG_ERROR(MODULE, "initDetector: model and metadata paths are required");
        return false;
    }

    std::string error;
    if (!rknn_postprocess::parseMetadata(config.metadataPath, s_metadata, error)) {
        VLOG_ERROR(MODULE, "metadata parse failed (%s): %s",
                   config.metadataPath.c_str(), error.c_str());
        clearDetectorState();
        return false;
    }

    std::string actualModelSha;
    if (!computeFileSha256(config.modelPath, actualModelSha, error)) {
        VLOG_ERROR(MODULE, "%s", error.c_str());
        clearDetectorState();
        return false;
    }
    VLOG_INFO(MODULE, "RKNN model SHA256: %s", actualModelSha.c_str());
    VLOG_INFO(MODULE, "RKNN metadata path: %s", config.metadataPath.c_str());
    if (!rknn_postprocess::validateMetadata(s_metadata, actualModelSha, error)) {
        VLOG_ERROR(MODULE, "metadata validation failed: %s", error.c_str());
        clearDetectorState();
        return false;
    }
    if (s_metadata.inputWidth != 640 || s_metadata.inputHeight != 640) {
        VLOG_ERROR(MODULE, "metadata input must be 640x640, got %dx%d",
                   s_metadata.inputWidth, s_metadata.inputHeight);
        clearDetectorState();
        return false;
    }

    s_rt = new rknn_runtime::Runtime();
    if (!s_rt->loadLibrary("librknnrt.so")) {
        VLOG_ERROR(MODULE, "loadLibrary failed: %s", s_rt->lastError().c_str());
        clearDetectorState();
        return false;
    }

    if (!s_rt->initModel(config.modelPath)) {
        VLOG_ERROR(MODULE, "initModel failed: %s", s_rt->lastError().c_str());
        clearDetectorState();
        return false;
    }

    // Query SDK version.
    rknn_sdk_version ver{};
    if (s_rt->querySdkVersion(ver)) {
        VLOG_INFO(MODULE, "RKNN API=%s driver=%s", ver.api_version, ver.drv_version);
    } else {
        VLOG_WARN(MODULE, "querySdkVersion failed: %s", s_rt->lastError().c_str());
    }

    // Query IO count.
    if (!s_rt->queryIoNum(s_ioNum)) {
        VLOG_ERROR(MODULE, "queryIoNum failed: %s", s_rt->lastError().c_str());
        clearDetectorState();
        return false;
    }
    if (s_ioNum.n_input != 1 || s_ioNum.n_output != 3) {
        VLOG_ERROR(MODULE, "expected exactly 1 input and 3 outputs, got %u and %u",
                   s_ioNum.n_input, s_ioNum.n_output);
        clearDetectorState();
        return false;
    }
    VLOG_INFO(MODULE, "RKNN model: %u inputs, %u outputs",
              s_ioNum.n_input, s_ioNum.n_output);

    // Query input attributes.
    s_inputAttrs.resize(s_ioNum.n_input);
    for (uint32_t i = 0; i < s_ioNum.n_input; ++i) {
        if (!s_rt->queryInputAttr(i, s_inputAttrs[i])) {
            VLOG_ERROR(MODULE, "queryInputAttr[%u] failed: %s", i, s_rt->lastError().c_str());
            clearDetectorState();
            return false;
        }
        VLOG_INFO(MODULE, "INPUT[%u]: name=%s fmt=%s type=%s qnt=%s n_dims=%u "
                  "n_elems=%u size=%u zp=%d scale=%.6f",
                  i, s_inputAttrs[i].name,
                  tensorFmtStr(s_inputAttrs[i].fmt),
                  tensorTypeStr(s_inputAttrs[i].type),
                  tensorQntStr(s_inputAttrs[i].qnt_type),
                  s_inputAttrs[i].n_dims, s_inputAttrs[i].n_elems,
                  s_inputAttrs[i].size, s_inputAttrs[i].zp,
                  s_inputAttrs[i].scale);
    }

    // Query output attributes.
    s_outputAttrs.resize(s_ioNum.n_output);
    for (uint32_t i = 0; i < s_ioNum.n_output; ++i) {
        if (!s_rt->queryOutputAttr(i, s_outputAttrs[i])) {
            VLOG_ERROR(MODULE, "queryOutputAttr[%u] failed: %s", i, s_rt->lastError().c_str());
            clearDetectorState();
            return false;
        }
        VLOG_INFO(MODULE, "OUTPUT[%u]: name=%s fmt=%s type=%s qnt=%s n_dims=%u "
                  "n_elems=%u size=%u zp=%d scale=%.6f",
                  i, s_outputAttrs[i].name,
                  tensorFmtStr(s_outputAttrs[i].fmt),
                  tensorTypeStr(s_outputAttrs[i].type),
                  tensorQntStr(s_outputAttrs[i].qnt_type),
                  s_outputAttrs[i].n_dims, s_outputAttrs[i].n_elems,
                  s_outputAttrs[i].size, s_outputAttrs[i].zp, s_outputAttrs[i].scale);
    }

    // Derive input geometry from input_attrs[0] (§P0-8).
    std::string dimErr;
    if (!deriveInputDims(s_inputAttrs[0], s_inputN, s_inputC, s_inputH, s_inputW, dimErr)) {
        VLOG_ERROR(MODULE, "unsupported input layout: %s", dimErr.c_str());
        clearDetectorState();
        return false;
    }
    s_inputFmt = s_inputAttrs[0].fmt;
    VLOG_INFO(MODULE, "RKNN input geometry: N=%u C=%u H=%u W=%u fmt=%s",
              s_inputN, s_inputC, s_inputH, s_inputW, tensorFmtStr(s_inputFmt));
    const uint64_t expectedInputElems = static_cast<uint64_t>(s_metadata.inputWidth) *
                                        s_metadata.inputHeight * s_metadata.inputChannels;
    const uint64_t expectedInputBytes = expectedInputElems * sizeof(uint16_t);
    if (s_inputAttrs[0].fmt != RKNN_TENSOR_NHWC ||
        s_inputAttrs[0].type != RKNN_TENSOR_FLOAT16 ||
        s_inputW != static_cast<uint32_t>(s_metadata.inputWidth) ||
        s_inputH != static_cast<uint32_t>(s_metadata.inputHeight) ||
        s_inputC != static_cast<uint32_t>(s_metadata.inputChannels) ||
        s_inputAttrs[0].n_elems != expectedInputElems ||
        s_inputAttrs[0].size != expectedInputBytes) {
        VLOG_ERROR(MODULE, "model input attrs do not match metadata native contract");
        clearDetectorState();
        return false;
    }
    std::string inputQuantizationError;
    if (!rknn_postprocess::validateFloatTensorQuantization(
            floatTensorQuantization(s_inputAttrs[0].qnt_type),
            s_inputAttrs[0].zp, s_inputAttrs[0].scale,
            inputQuantizationError)) {
        VLOG_ERROR(MODULE,
                   "model input quantization rejected: qnt=%s zp=%d scale=%.9g: %s",
                   tensorQntStr(s_inputAttrs[0].qnt_type), s_inputAttrs[0].zp,
                   static_cast<double>(s_inputAttrs[0].scale),
                   inputQuantizationError.c_str());
        clearDetectorState();
        return false;
    }

    std::set<int> actualStrides;
    for (uint32_t i = 0; i < s_ioNum.n_output; ++i) {
        std::string outputError;
        const int stride = validateOutputAttr(s_outputAttrs[i], outputError);
        if (stride == 0 || !actualStrides.insert(stride).second) {
            VLOG_ERROR(MODULE, "output[%u] invalid or duplicate: %s", i,
                       outputError.empty() ? "duplicate stride" : outputError.c_str());
            clearDetectorState();
            return false;
        }
        VLOG_INFO(MODULE, "RKNN output[%u]: grid=%ux%u derived_stride=%d",
                  i, s_outputAttrs[i].dims[3], s_outputAttrs[i].dims[2], stride);
    }
    const std::set<int> requiredStrides(s_metadata.strides.begin(),
                                        s_metadata.strides.end());
    if (requiredStrides != std::set<int>({8, 16, 32}) ||
        actualStrides != requiredStrides) {
        VLOG_ERROR(MODULE, "model outputs do not provide exact metadata strides {8,16,32}");
        clearDetectorState();
        return false;
    }

    // Allocate RGB888 input buffer (W*H*3) — NOT W*H*4 (§P0-8, §P0-9).
    size_t inputBytes = 0;
    if (!safeMul3(s_inputW, s_inputH, s_inputC, inputBytes) || inputBytes == 0) {
        VLOG_ERROR(MODULE, "input size overflow: W=%u H=%u C=%u",
                   s_inputW, s_inputH, s_inputC);
        clearDetectorState();
        return false;
    }
    s_inputBuffer.assign(inputBytes, 0);

    s_config = config;
    s_config.modelWidth  = static_cast<int>(s_inputW);
    s_config.modelHeight = static_cast<int>(s_inputH);

    // Lazily create the decode thread pool (3 workers for the 3 strides).
    // Recreate if a previous init left one behind with a different config.
    if (!s_decodePool || s_decodePool->statistics().workerCount != 3) {
        s_decodePool = std::make_unique<concurrency::ThreadPool>(3);
    }
    s_detectorReady = true;

    VLOG_INFO(MODULE, "RKNN YOLOv5 detector initialized: %s (%ux%u, conf=%.2f, nms=%.2f)",
              config.modelPath.c_str(), s_inputW, s_inputH,
              config.confThreshold, config.nmsThreshold);
    return true;
}

void rknn::deinitDetector()
{
    clearDetectorState();
    VLOG_INFO(MODULE, "RKNN detector released");
}

bool rknn::detect(const uint8_t *rgbData, int imgWidth, int imgHeight,
                  std::vector<Detection> &results)
{
    results.clear();

    if (!s_detectorReady || !s_rt || !rgbData || s_inputBuffer.empty())
        return false;

    if (imgWidth <= 0 || imgHeight <= 0) {
        VLOG_ERROR(MODULE, "detect: invalid image dims %dx%d", imgWidth, imgHeight);
        return false;
    }

    // Step 1: explicit RGB32 → RGB888 conversion (§P0-9).
    //   Caller (Monitor.cpp) passes QImage::Format_RGB32 bits, which on
    //   little-endian ARM is laid out as B,G,R,X. We must NOT feed that
    //   directly to the model.
    //
    //   For Stage A (rknn_enabled=false), this path is dormant. For Stage B
    //   (static image), the test harness will pass pre-converted RGB888 via
    //   the same API; until then, we still do the conversion defensively.
    if (s_inputC != 3) {
        VLOG_ERROR(MODULE, "detect: model input channels != 3 (got %u)", s_inputC);
        return false;
    }

    size_t sourceBytes = 0;
    if (!safeMul3(static_cast<uint32_t>(imgWidth),
                  static_cast<uint32_t>(imgHeight), 3, sourceBytes)) {
        VLOG_ERROR(MODULE, "detect: source image size overflow");
        return false;
    }
    std::vector<uint8_t> srcRGB888(sourceBytes);
    convertRGB32toRGB888(rgbData, imgWidth, imgHeight, srcRGB888.data());
    const rknn_postprocess::LetterboxParams letterbox =
        rknn_postprocess::computeLetterbox(imgWidth, imgHeight,
                                           static_cast<int>(s_inputW),
                                           static_cast<int>(s_inputH));
    if (letterbox.scale <= 0.0f ||
        !rknn_postprocess::applyLetterbox(
            srcRGB888.data(), imgWidth, imgHeight, s_inputBuffer.data(),
            static_cast<int>(s_inputW), static_cast<int>(s_inputH), letterbox,
            static_cast<uint8_t>(rknn_postprocess::kLetterboxFill))) {
        VLOG_ERROR(MODULE, "detect: shared letterbox preprocessing failed");
        return false;
    }

    // Step 2: fill rknn_input with OFFICIAL field layout (§P0-5).
    rknn_input input{};
    input.index        = 0;
    input.buf          = s_inputBuffer.data();
    input.size         = static_cast<uint32_t>(s_inputBuffer.size());
    input.pass_through = static_cast<uint8_t>(s_metadata.callerInputPassThrough);
    input.type         = RKNN_TENSOR_UINT8;
    input.fmt          = RKNN_TENSOR_NHWC;

    int rc = s_rt->inputsSet(1, &input);
    if (rc != RKNN_SUCC) {
        VLOG_ERROR(MODULE, "rknn_inputs_set failed: rc=%d", rc);
        return false;
    }

    // Step 3: explicit rknn_run (§P0-3). NEVER assume outputs_get runs it.
    rc = s_rt->run();
    if (rc != RKNN_SUCC) {
        VLOG_ERROR(MODULE, "rknn_run failed: rc=%d", rc);
        return false;
    }

    // Step 4: prepare rknn_output array with OFFICIAL layout (§P0-6).
    std::vector<rknn_output> outputs(s_ioNum.n_output);
    for (uint32_t i = 0; i < s_ioNum.n_output; ++i) {
        outputs[i].want_float  = 1;   // request float — decoder assumes float
        outputs[i].is_prealloc = 0;
        outputs[i].index       = i;
        outputs[i].buf         = nullptr;
        outputs[i].size        = 0;
    }

    // Step 5: outputs_get — 4th arg nullptr (§P0-4, handled in Runtime).
    rc = s_rt->outputsGet(s_ioNum.n_output, outputs.data());
    if (rc != RKNN_SUCC) {
        VLOG_ERROR(MODULE, "rknn_outputs_get failed: rc=%d", rc);
        return false;
    }

    // Step 6: validate + decode outputs. RAII release guard ensures
    // outputs_release is always called (§四.16).
    struct OutputReleaseGuard {
        rknn_runtime::Runtime *rt;
        uint32_t                n;
        rknn_output            *outs;
        ~OutputReleaseGuard() { if (rt && outs) rt->outputsRelease(n, outs); }
    } guard{s_rt, s_ioNum.n_output, outputs.data()};

    std::vector<rknn_postprocess::OutputTensorView> outputViews;
    outputViews.reserve(s_ioNum.n_output);
    for (uint32_t i = 0; i < s_ioNum.n_output; ++i) {
        const size_t requiredBytes =
            static_cast<size_t>(s_outputAttrs[i].n_elems) * sizeof(float);
        if (!outputs[i].buf || outputs[i].size < requiredBytes) {
            VLOG_ERROR(MODULE, "output[%u] is null or too small: %u < %llu",
                       i, outputs[i].size,
                       static_cast<unsigned long long>(requiredBytes));
            return false;
        }
        rknn_postprocess::OutputTensorView view;
        view.data = static_cast<const float *>(outputs[i].buf);
        view.byteSize = static_cast<size_t>(outputs[i].size);
        view.gridWidth = static_cast<int>(s_outputAttrs[i].dims[3]);
        view.gridHeight = static_cast<int>(s_outputAttrs[i].dims[2]);
        view.outputIndex = static_cast<int>(outputs[i].index);
        outputViews.push_back(view);
    }

    std::vector<rknn_postprocess::Detection> decoded;
    std::string decodeError;
    // Parallel decode: 3 YOLOv5 output layers (80x80 / 40x40 / 20x20) are
    // dispatched to s_decodePool and awaited via std::future. NMS stays
    // serial. Falls back to serial decodeOutputs() if the pool was not
    // initialized (defensive — should not happen after a successful init).
    bool decodeOk;
    if (s_decodePool) {
        decodeOk = rknn_postprocess::decodeOutputsParallel(
            outputViews, s_metadata, letterbox, s_config.confThreshold,
            s_config.nmsThreshold, decoded, decodeError, *s_decodePool);
    } else {
        decodeOk = rknn_postprocess::decodeOutputs(
            outputViews, s_metadata, letterbox, s_config.confThreshold,
            s_config.nmsThreshold, decoded, decodeError);
    }
    if (!decodeOk) {
        VLOG_ERROR(MODULE, "shared RKNN postprocess failed: %s", decodeError.c_str());
        return false;
    }

    results.reserve(decoded.size());
    for (const rknn_postprocess::Detection &src : decoded) {
        rknn_postprocess::Box normalized;
        std::string boxError;
        if (!std::isfinite(src.confidence) ||
            src.confidence < 0.0f || src.confidence > 1.0f ||
            src.classIndex < 0 || src.classIndex >= COCO_CLASS_COUNT) {
            VLOG_ERROR(MODULE, "shared RKNN postprocess returned invalid detection");
            results.clear();
            return false;
        }
        if (!rknn_postprocess::validateAndNormalizeBox(
                src.box, imgWidth, imgHeight, normalized, boxError)) {
            VLOG_ERROR(MODULE, "shared RKNN box rejected: %s", boxError.c_str());
            results.clear();
            return false;
        }
        rknn::Detection dst;
        dst.x = normalized.cx;
        dst.y = normalized.cy;
        dst.w = normalized.w;
        dst.h = normalized.h;
        dst.confidence = src.confidence;
        dst.classId = src.classIndex;
        results.push_back(dst);
    }

    // Only log when detections are found to avoid log flooding at 6 Hz
    // (4 empty detections per second would pollute the log otherwise).
    if (!results.empty()) {
        VLOG_DEBUG(MODULE, "RKNN detect: %d objects found",
                   static_cast<int>(results.size()));
    }
    return true;
}

void rknn::drawDetections(uint8_t *rgbData, int imgWidth, int imgHeight,
                          const std::vector<Detection> &detections,
                          const std::vector<std::string> &classNames)
{
    static const uint8_t COLORS[][3] = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255},
        {255, 255, 0}, {255, 0, 255}, {0, 255, 255},
        {128, 0, 0}, {0, 128, 0}, {0, 0, 128}, {128, 128, 0},
    };
    static const int COLOR_COUNT = 10;

    for (const auto &det : detections) {
        int colorIdx = det.classId % COLOR_COUNT;
        uint8_t r = COLORS[colorIdx][0];
        uint8_t g = COLORS[colorIdx][1];
        uint8_t b = COLORS[colorIdx][2];

        // Detection.x/y are CENTER-point normalized (§五.8).
        int cx = static_cast<int>(det.x * imgWidth);
        int cy = static_cast<int>(det.y * imgHeight);
        int w  = static_cast<int>(det.w * imgWidth);
        int h  = static_cast<int>(det.h * imgHeight);

        int x1 = cx - w / 2;
        int y1 = cy - h / 2;
        int x2 = cx + w / 2;
        int y2 = cy + h / 2;

        // Draw hollow border (2px).
        auto drawBorder = [&](int x1, int y1, int x2, int y2) {
            for (int t = 0; t < 2; ++t) {
                for (int col = x1; col <= x2; ++col) {
                    if (y1 + t >= 0 && y1 + t < imgHeight && col >= 0 && col < imgWidth) {
                        int idx = ((y1 + t) * imgWidth + col) * 4;
                        rgbData[idx + 0] = b; rgbData[idx + 1] = g; rgbData[idx + 2] = r;
                    }
                    if (y2 - t >= 0 && y2 - t < imgHeight && col >= 0 && col < imgWidth) {
                        int idx = ((y2 - t) * imgWidth + col) * 4;
                        rgbData[idx + 0] = b; rgbData[idx + 1] = g; rgbData[idx + 2] = r;
                    }
                }
                for (int row = y1; row <= y2; ++row) {
                    if (row >= 0 && row < imgHeight && x1 + t >= 0 && x1 + t < imgWidth) {
                        int idx = (row * imgWidth + (x1 + t)) * 4;
                        rgbData[idx + 0] = b; rgbData[idx + 1] = g; rgbData[idx + 2] = r;
                    }
                    if (row >= 0 && row < imgHeight && x2 - t >= 0 && x2 - t < imgWidth) {
                        int idx = (row * imgWidth + (x2 - t)) * 4;
                        rgbData[idx + 0] = b; rgbData[idx + 1] = g; rgbData[idx + 2] = r;
                    }
                }
            }
        };
        drawBorder(x1, y1, x2, y2);
        // (Label text drawing intentionally omitted in this Stage A refactor;
        // existing font helpers were tied to the hand-written ABI era. The
        // Monitor page will receive boxes only; label text returns in Stage B.)
        (void)classNames;
    }
}

const std::vector<std::string> &rknn::cocoClassNames()
{
    static std::vector<std::string> names(COCO_NAMES, COCO_NAMES + COCO_CLASS_COUNT);
    return names;
}

#else
// ============================================================================
// Disabled stub — no official rknn_api.h available (Windows host builds).
// Per jilu.txt §六.1: "禁止在无官方头文件时回退到手写 ABI."
// All functions return failure / empty; the app builds but RKNN is disabled.
// ============================================================================

bool rknn::initDetector(const DetectorConfig &) { return false; }
void rknn::deinitDetector() {}
bool rknn::detect(const uint8_t *, int, int, std::vector<Detection> &)
{ return false; }
void rknn::drawDetections(uint8_t *, int, int,
                          const std::vector<Detection> &,
                          const std::vector<std::string> &) {}
const std::vector<std::string> &rknn::cocoClassNames()
{
    static const std::vector<std::string> empty;
    return empty;
}
#endif // __has_include(<rknn_api.h>)

// ----------------------------------------------------------------------------
// Configuration loading (host-safe, no RKNN dependency)
// ----------------------------------------------------------------------------

rknn::DetectorConfig rknn::loadDetectorConfigFile(const std::string &path,
                                                   bool *loaded,
                                                   std::string *errorMessage)
{
    DetectorConfig config;

    std::ifstream file(path);
    if (!file.is_open()) {
        if (loaded) *loaded = false;
        if (errorMessage) *errorMessage = "config file not found: " + path;
        VLOG_WARN(MODULE, "RKNN config not found: %s, using defaults", path.c_str());
        return config;
    }

    std::string line;
    while (std::getline(file, line)) {
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);

        if (line.empty() || line[0] == '#') continue;

        size_t eqPos = line.find('=');
        if (eqPos == std::string::npos) continue;

        std::string key = line.substr(0, eqPos);
        std::string value = line.substr(eqPos + 1);

        auto trim = [](std::string &s) {
            size_t p = s.find_first_not_of(" \t");
            if (p != std::string::npos) s = s.substr(p);
            p = s.find_last_not_of(" \t");
            if (p != std::string::npos) s = s.substr(0, p + 1);
        };
        trim(key);
        trim(value);

        try {
            if (key == "rknn_enabled") {
                config.enabled = (value == "true" || value == "1");
            } else if (key == "rknn_model_path") {
                config.modelPath = value;
            } else if (key == "rknn_metadata_path") {
                if (!value.empty()) config.metadataPath = value;
            } else if (key == "rknn_model_width") {
                int v = std::stoi(value);
                if (v > 0) config.modelWidth = v;
            } else if (key == "rknn_model_height") {
                int v = std::stoi(value);
                if (v > 0) config.modelHeight = v;
            } else if (key == "rknn_conf_threshold") {
                float v = std::stof(value);
                if (v > 0.0f && v < 1.0f) config.confThreshold = v;
            } else if (key == "rknn_nms_threshold") {
                float v = std::stof(value);
                if (v > 0.0f && v < 1.0f) config.nmsThreshold = v;
            }
        } catch (const std::exception &e) {
            VLOG_WARN(MODULE, "RKNN config parse error on '%s=%s': %s",
                      key.c_str(), value.c_str(), e.what());
        }
    }
    file.close();

    if (loaded) *loaded = true;
    VLOG_INFO(MODULE, "RKNN config loaded: enabled=%d model=%s metadata=%s %dx%d conf=%.2f nms=%.2f",
              config.enabled ? 1 : 0, config.modelPath.c_str(), config.metadataPath.c_str(),
              config.modelWidth, config.modelHeight,
              config.confThreshold, config.nmsThreshold);
    return config;
}
