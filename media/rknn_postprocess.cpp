// rknn_postprocess 閳?pure C++17 implementation of Stage B helpers.
//
// Implements the interfaces declared in rknn_postprocess.h:
//   * metadata parse/validate (jilu.txt 鎼?)
//   * letterbox forward/reverse (jilu.txt 鎼?)
//   * YOLOv5 v6 NCHW channel-major decode (jilu.txt 鎼?, line 397-402)
//   * class-aware NMS (jilu.txt 鎼俆DD item 10-11)
//   * expect-class validation (jilu.txt 鎼俆DD item 14)
//
// No Qt, no NPU, no OpenCV 閳?host-compilable for unit testing.

#include "rknn_postprocess.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

#include "../common/thread_pool.h"

namespace rknn_postprocess {

bool validateFloatTensorQuantization(FloatTensorQuantization quantization,
                                     int zeroPoint, float scale,
                                     std::string &error)
{
    error.clear();
    if (quantization == FloatTensorQuantization::None) {
        return true;
    }
    if (quantization == FloatTensorQuantization::Dfp) {
        error = "DFP quantization is not supported for a float tensor";
        return false;
    }
    if (quantization != FloatTensorQuantization::Affine) {
        error = "unsupported quantization for a float tensor";
        return false;
    }
    if (zeroPoint != 0) {
        error = "identity AFFINE requires zero point 0";
        return false;
    }
    if (!std::isfinite(scale)) {
        error = "identity AFFINE requires a finite scale";
        return false;
    }
    constexpr float tolerance = 4.0f * std::numeric_limits<float>::epsilon();
    if (std::fabs(scale - 1.0f) > tolerance) {
        error = "identity AFFINE requires unit scale 1";
        return false;
    }
    return true;
}

// ============================================================
// Internal helpers
// ============================================================
namespace {

// Trim leading/trailing whitespace.
std::string trim(const std::string &s)
{
    const char *ws = " \t\r\n";
    auto a = s.find_first_not_of(ws);
    if (a == std::string::npos) return std::string();
    auto b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

// Parse a comma-separated integer list. Returns false on parse error.
bool parseIntList(const std::string &s, std::vector<int> &out)
{
    out.clear();
    std::string token;
    std::stringstream ss(s);
    while (std::getline(ss, token, ',')) {
        token = trim(token);
        if (token.empty()) continue;
        try {
            int v = std::stoi(token);
            out.push_back(v);
        } catch (...) {
            return false;
        }
    }
    return true;
}

// Check a float for NaN/Inf.
bool isFiniteSafe(float x)
{
    return std::isfinite(x);
}

} // namespace

// ============================================================
// Metadata (jilu.txt 鎼?)
// ============================================================
bool parseMetadata(const std::string &path, Metadata &out, std::string &err)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        err = "cannot open metadata file: " + path;
        return false;
    }

    // Start from a clean slate (preserve defaults from struct decl).
    out = Metadata{};

    std::string line;
    int lineNo = 0;
    while (std::getline(f, line)) {
        ++lineNo;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;

        auto eq = t.find('=');
        if (eq == std::string::npos) {
            err = "metadata line " + std::to_string(lineNo) +
                  ": missing '='";
            return false;
        }
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        if (key.empty()) {
            err = "metadata line " + std::to_string(lineNo) +
                  ": empty key";
            return false;
        }

        if (key == "model_sha256")            out.modelSha256 = val;
        else if (key == "onnx_sha256")        out.onnxSha256 = val;
        else if (key == "provenance")         out.provenance = val;
        else if (key == "labels_file")        out.labelsFile = val;
        else if (key == "decode_variant")     out.decodeVariant = val;
        else if (key == "input_width")        out.inputWidth = std::atoi(val.c_str());
        else if (key == "input_height")       out.inputHeight = std::atoi(val.c_str());
        else if (key == "input_channels")     out.inputChannels = std::atoi(val.c_str());
        else if (key == "input_layout")       out.inputLayout = val;
        else if (key == "input_type")         out.inputType = val;
        else if (key == "caller_input_fmt")   out.callerInputFormat = val;
        else if (key == "caller_input_type")  out.callerInputType = val;
        else if (key == "caller_input_pass_through") out.callerInputPassThrough = std::atoi(val.c_str());
        else if (key == "class_count")        out.classCount = std::atoi(val.c_str());
        else if (key == "channel_count")      out.channelCount = std::atoi(val.c_str());
        else if (key == "output_count")       out.outputCount = std::atoi(val.c_str());
        else if (key == "output_layout")      out.outputLayout = val;
        else if (key == "output_type")        out.outputType = val;
        else if (key == "output_request")     out.outputRequest = val;
        else if (key == "output_0_dims")      parseIntList(val, out.outputDims[0]);
        else if (key == "output_1_dims")      parseIntList(val, out.outputDims[1]);
        else if (key == "output_2_dims")      parseIntList(val, out.outputDims[2]);
        else if (key == "strides") {
            if (!parseIntList(val, out.strides)) {
                err = "metadata line " + std::to_string(lineNo) +
                      ": invalid strides list";
                return false;
            }
        }
        else if (key == "anchors_stride8") {
            if (!parseIntList(val, out.anchorsStride8)) {
                err = "metadata line " + std::to_string(lineNo) +
                      ": invalid anchors_stride8";
                return false;
            }
        }
        else if (key == "anchors_stride16") {
            if (!parseIntList(val, out.anchorsStride16)) {
                err = "metadata line " + std::to_string(lineNo) +
                      ": invalid anchors_stride16";
                return false;
            }
        }
        else if (key == "anchors_stride32") {
            if (!parseIntList(val, out.anchorsStride32)) {
                err = "metadata line " + std::to_string(lineNo) +
                      ": invalid anchors_stride32";
                return false;
            }
        }
        else if (key == "golden_image")            out.goldenImage = val;
        else if (key == "golden_image_sha256")     out.goldenImageSha256 = val;
        else if (key == "expected_class")          out.expectedClass = val;
        else if (key == "expected_class_index")    out.expectedClassIndex = std::atoi(val.c_str());
        else if (key == "output_sigmoid")          out.outputSigmoidBuiltin = (val == "builtin");
        else if (key == "grid_sensitivity")        out.gridSensitivity = std::strtof(val.c_str(), nullptr);
        else if (key == "letterbox_fill")          out.letterboxFill = std::atoi(val.c_str());
        // Unknown keys are silently ignored (forward-compat).
    }
    return true;
}

bool validateMetadata(const Metadata &m, const std::string &actualModelSha256,
                      std::string &err)
{
    err.clear();

    // SHA match (jilu.txt 鎼?.1).
    if (m.modelSha256.empty()) {
        err = "metadata model_sha256 is empty";
        return false;
    }
    if (m.modelSha256 != actualModelSha256) {
        err = "metadata model_sha256 mismatch: meta=" + m.modelSha256 +
              " actual=" + actualModelSha256;
        return false;
    }

    // class_count must be 80 (jilu.txt 鎼?.2).
    if (m.classCount != kCocoClassCount) {
        err = "metadata class_count must be " +
              std::to_string(kCocoClassCount) + ", got " +
              std::to_string(m.classCount);
        return false;
    }

    // strides must be {8, 16, 32}.
    if (m.strides.size() != 3 ||
        m.strides[0] != 8 || m.strides[1] != 16 || m.strides[2] != 32) {
        err = "metadata strides must be {8,16,32}";
        return false;
    }

    // Anchors: 6 values per stride.
    if (m.anchorsStride8.size() != 6 ||
        m.anchorsStride16.size() != 6 ||
        m.anchorsStride32.size() != 6) {
        err = "metadata anchors must have 6 values per stride";
        return false;
    }

    if (m.inputWidth != 640 || m.inputHeight != 640 || m.inputChannels != 3 ||
        m.inputLayout != "NHWC" || m.inputType != "FLOAT16") {
        err = "metadata native input must be NHWC FLOAT16 640x640x3";
        return false;
    }
    if (m.callerInputFormat != "NHWC") {
        err = "metadata caller_input_fmt must be NHWC";
        return false;
    }
    if (m.callerInputType != "UINT8") {
        err = "metadata caller_input_type must be UINT8";
        return false;
    }
    if (m.callerInputPassThrough != 0) {
        err = "metadata caller_input_pass_through must be 0";
        return false;
    }
    const std::vector<int> expectedDims[3] = {
        {1, 255, 80, 80}, {1, 255, 40, 40}, {1, 255, 20, 20}};
    if (m.outputCount != 3 || m.outputLayout != "NCHW" ||
        m.outputType != "FLOAT16" || m.outputRequest != "FLOAT32" ||
        m.channelCount != kChannelCount ||
        m.outputDims[0] != expectedDims[0] ||
        m.outputDims[1] != expectedDims[1] ||
        m.outputDims[2] != expectedDims[2]) {
        err = "metadata output contract must be three FLOAT16 NCHW 255-channel grids";
        return false;
    }
    if (m.decodeVariant != "yolov5_v6" || !m.outputSigmoidBuiltin ||
        std::fabs(m.gridSensitivity - 2.0f) > 1e-6f ||
        m.letterboxFill != 114) {
        err = "metadata decode contract requires yolov5_v6 builtin output_sigmoid, grid_sensitivity=2.0 and letterbox_fill=114";
        return false;
    }
    const auto positiveAnchors = [](const std::vector<int> &anchors) {
        return anchors.size() == 6 &&
               std::all_of(anchors.begin(), anchors.end(),
                           [](int value) { return value > 0; });
    };
    if (!positiveAnchors(m.anchorsStride8) ||
        !positiveAnchors(m.anchorsStride16) ||
        !positiveAnchors(m.anchorsStride32)) {
        err = "metadata anchors must contain six positive values per stride";
        return false;
    }

    return true;
}

bool loadLabels(const std::string &path, std::vector<std::string> &out,
                std::string &err)
{
    out.clear();
    std::ifstream f(path);
    if (!f.is_open()) {
        err = "cannot open labels file: " + path;
        return false;
    }
    std::string line;
    while (std::getline(f, line)) {
        // Strip trailing CR (CRLF files).
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Skip truly empty lines but keep labels that contain spaces.
        if (line.empty()) continue;
        out.push_back(line);
    }
    return true;
}

int findClassIndexByName(const std::vector<std::string> &labels,
                         const std::string &name)
{
    const auto it = std::find(labels.begin(), labels.end(), name);
    return it == labels.end()
        ? -1
        : static_cast<int>(std::distance(labels.begin(), it));
}

// ============================================================
// Letterbox (jilu.txt 鎼?)
// ============================================================
LetterboxParams computeLetterbox(int srcW, int srcH, int dstW, int dstH)
{
    LetterboxParams p;
    p.srcW = srcW;
    p.srcH = srcH;
    p.dstW = dstW;
    p.dstH = dstH;

    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
        p.scale = 1.0f;
        p.padLeft = p.padRight = p.padTop = p.padBottom = 0;
        return p;
    }

    float sx = static_cast<float>(dstW) / static_cast<float>(srcW);
    float sy = static_cast<float>(dstH) / static_cast<float>(srcH);
    p.scale = std::min(sx, sy);

    int newW = static_cast<int>(std::lround(srcW * p.scale));
    int newH = static_cast<int>(std::lround(srcH * p.scale));
    if (newW > dstW)  newW = dstW;
    if (newH > dstH)  newH = dstH;
    if (newW < 0) newW = 0;
    if (newH < 0) newH = 0;

    int totalPadX = dstW - newW;
    int totalPadY = dstH - newH;
    // Left-pad = floor(total/2), right-pad = ceil(total/2).
    // This guarantees padLeft + padRight == totalPadX even for odd totals
    // (jilu.txt 鎼俆DD item 6: even/odd padding handled).
    p.padLeft   = totalPadX / 2;
    p.padRight  = totalPadX - p.padLeft;
    p.padTop    = totalPadY / 2;
    p.padBottom = totalPadY - p.padTop;

    return p;
}

bool applyLetterbox(const uint8_t *src, int srcW, int srcH,
                    uint8_t *dst, int dstW, int dstH,
                    const LetterboxParams &p, uint8_t fill)
{
    if (!src || !dst) return false;
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return false;
    if (p.scale <= 0.0f) return false;

    // Fill destination with `fill`.
    std::memset(dst, fill, static_cast<size_t>(dstW) * dstH * 3);

    // Determine the scaled region bounds in the destination.
    int newW = dstW - p.padLeft - p.padRight;
    int newH = dstH - p.padTop - p.padBottom;
    if (newW <= 0 || newH <= 0) return false;

    // Nearest-neighbor resample. For each destination pixel in the scaled
    // region, compute the source pixel index. Avoid floating-point error by
    // mapping via inverse scale.
    float invScale = 1.0f / p.scale;
    for (int dy = 0; dy < newH; ++dy) {
        int sy = static_cast<int>((dy + 0.5f) * invScale);
        if (sy < 0) sy = 0;
        if (sy >= srcH) sy = srcH - 1;
        uint8_t *dRow = dst + ((dy + p.padTop) * dstW + p.padLeft) * 3;
        const uint8_t *sRow = src + sy * srcW * 3;
        for (int dx = 0; dx < newW; ++dx) {
            int sx = static_cast<int>((dx + 0.5f) * invScale);
            if (sx < 0) sx = 0;
            if (sx >= srcW) sx = srcW - 1;
            const uint8_t *sPx = sRow + sx * 3;
            uint8_t *dPx = dRow + dx * 3;
            dPx[0] = sPx[0];
            dPx[1] = sPx[1];
            dPx[2] = sPx[2];
        }
    }
    return true;
}

Box reverseLetterbox(Box modelBox, const LetterboxParams &p)
{
    // Step 1: model coords -> original coords (no clipping yet).
    float invScale = (p.scale > 0.0f) ? (1.0f / p.scale) : 1.0f;
    Box orig;
    orig.cx = (modelBox.cx - static_cast<float>(p.padLeft)) * invScale;
    orig.cy = (modelBox.cy - static_cast<float>(p.padTop))  * invScale;
    orig.w  = modelBox.w * invScale;
    orig.h  = modelBox.h * invScale;

    // Step 2: convert to corner coords (x1,y1,x2,y2).
    float halfW = orig.w * 0.5f;
    float halfH = orig.h * 0.5f;
    float x1 = orig.cx - halfW;
    float y1 = orig.cy - halfH;
    float x2 = orig.cx + halfW;
    float y2 = orig.cy + halfH;

    // Step 3: clip to [0, srcW] x [0, srcH] (jilu.txt 鎼俆DD item 8).
    const float maxX = static_cast<float>(std::max(p.srcW, 0));
    const float maxY = static_cast<float>(std::max(p.srcH, 0));
    x1 = std::clamp(x1, 0.0f, maxX);
    y1 = std::clamp(y1, 0.0f, maxY);
    x2 = std::clamp(x2, 0.0f, maxX);
    y2 = std::clamp(y2, 0.0f, maxY);
    if (x2 < x1) x2 = x1;
    if (y2 < y1) y2 = y1;

    // Step 4: back to center format.
    orig.cx = (x1 + x2) * 0.5f;
    orig.cy = (y1 + y2) * 0.5f;
    orig.w  = x2 - x1;
    orig.h  = y2 - y1;
    return orig;
}

bool validateAndNormalizeBox(const Box &box, int imageWidth, int imageHeight,
                             Box &normalized, std::string &error)
{
    normalized = Box{};
    error.clear();
    if (imageWidth <= 0 || imageHeight <= 0 ||
        !std::isfinite(box.cx) || !std::isfinite(box.cy) ||
        !std::isfinite(box.w) || !std::isfinite(box.h) ||
        box.w <= 0.0f || box.h <= 0.0f) {
        error = "box geometry must be finite and strictly positive";
        return false;
    }
    float x1 = box.cx - box.w * 0.5f;
    float y1 = box.cy - box.h * 0.5f;
    float x2 = box.cx + box.w * 0.5f;
    float y2 = box.cy + box.h * 0.5f;
    const float scale = std::max({1.0f, static_cast<float>(imageWidth),
                                  static_cast<float>(imageHeight),
                                  std::fabs(x1), std::fabs(y1),
                                  std::fabs(x2), std::fabs(y2)});
    const float epsilon = 8.0f * std::numeric_limits<float>::epsilon() * scale;
    if (x1 < -epsilon || y1 < -epsilon ||
        x2 > static_cast<float>(imageWidth) + epsilon ||
        y2 > static_cast<float>(imageHeight) + epsilon) {
        error = "box edges exceed original image bounds";
        return false;
    }
    x1 = std::clamp(x1, 0.0f, static_cast<float>(imageWidth));
    y1 = std::clamp(y1, 0.0f, static_cast<float>(imageHeight));
    x2 = std::clamp(x2, 0.0f, static_cast<float>(imageWidth));
    y2 = std::clamp(y2, 0.0f, static_cast<float>(imageHeight));
    if (x2 <= x1 || y2 <= y1) {
        error = "clamped box has zero area";
        return false;
    }
    const float invW = 1.0f / static_cast<float>(imageWidth);
    const float invH = 1.0f / static_cast<float>(imageHeight);
    normalized.cx = std::clamp((x1 + x2) * 0.5f * invW, 0.0f, 1.0f);
    normalized.cy = std::clamp((y1 + y2) * 0.5f * invH, 0.0f, 1.0f);
    normalized.w = std::clamp((x2 - x1) * invW, 0.0f, 1.0f);
    normalized.h = std::clamp((y2 - y1) * invH, 0.0f, 1.0f);
    return true;
}

// ============================================================
// YOLO output layer info (jilu.txt 鎼?)
// ============================================================
int strideFromGrid(int gridW, int gridH)
{
    // YOLOv5 has square grids; reject non-square as a safety check.
    if (gridW != gridH) return 0;
    switch (gridW) {
        case 80: return 8;
        case 40: return 16;
        case 20: return 32;
        default: return 0;
    }
}

bool buildYoloLayer(int gridW, int gridH, int stride, int nElems,
                    int channelCount, int outputIndex, YoloLayerInfo &out,
                    std::string &err)
{
    err.clear();
    if (gridW <= 0 || gridH <= 0 || stride <= 0 || channelCount <= 0) {
        err = "invalid YOLO layer geometry";
        return false;
    }
    // Stage B only supports YOLOv5s COCO 80-class with 255 channels
    // (jilu.txt 鎼傛潏鎾冲毉鐢啫鐪宀冪槈: channel == 255, 鎼俆DD item 3).
    if (channelCount != kChannelCount) {
        err = "channelCount must be " + std::to_string(kChannelCount) +
              " (3*(5+80)), got " + std::to_string(channelCount);
        return false;
    }
    const std::size_t width = static_cast<std::size_t>(gridW);
    const std::size_t height = static_cast<std::size_t>(gridH);
    const std::size_t channels = static_cast<std::size_t>(channelCount);
    const std::size_t sizeMax = std::numeric_limits<std::size_t>::max();
    if (width > sizeMax / height || width * height > sizeMax / channels) {
        err = "YOLO layer element count overflows size_t";
        return false;
    }
    const std::size_t expected = width * height * channels;
    if (expected > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        err = "YOLO layer element count exceeds INT_MAX";
        return false;
    }
    if (nElems < 0 || static_cast<std::size_t>(nElems) != expected) {
        err = "nElems mismatch: got " + std::to_string(nElems) +
              " expected " + std::to_string(expected);
        return false;
    }
    out.gridW  = gridW;
    out.gridH  = gridH;
    out.stride = stride;
    out.nElems = nElems;
    out.index  = outputIndex;
    return true;
}

// ============================================================
// Math helpers
// ============================================================
float sigmoid(float x)
{
    // Standard logistic sigmoid. expf naturally handles +/-Inf and NaN.
    return 1.0f / (1.0f + std::exp(-x));
}

// ============================================================
// YOLOv5 v6 NCHW channel-major decode (jilu.txt 鎼?)
// ============================================================
bool decodeYoloLayer(const float *data, const YoloLayerInfo &info,
                     const int anchors[6], int classCount,
                     float confThreshold, bool outputAlreadySigmoided,
                     std::vector<Detection> &out,
                     std::string &err)
{
    err.clear();
    if (!data) { err = "null data"; return false; }
    if (classCount != kCocoClassCount) {
        err = "decodeYoloLayer only supports class_count=80";
        return false;
    }
    if (info.gridW <= 0 || info.gridH <= 0 || info.stride <= 0 ||
        info.nElems <= 0) {
        err = "invalid YoloLayerInfo";
        return false;
    }

    // ---- Step 1: scan the entire buffer for NaN/Inf (jilu.txt 鎼俆DD item 9).
    // Any non-finite value invalidates the whole layer 閳?the runtime is
    // supposed to produce clean FLOAT32 from want_float=1; NaN/Inf would
    // indicate a model/runtime mismatch.
    for (int i = 0; i < info.nElems; ++i) {
        if (!isFiniteSafe(data[i])) {
            err = "decodeYoloLayer: NaN/Inf at index " + std::to_string(i);
            return false;
        }
    }

    const int gridW   = info.gridW;
    const int gridH   = info.gridH;
    const int stride  = info.stride;
    const int chanPerAnchor = 5 + classCount;       // 85

    // Helper: index into the NCHW channel-major buffer.
    //   channel = anchor * (5 + classCount) + component
    //   index   = (channel * gridH + row) * gridW + col
    auto at = [&](int anchor, int component, int row, int col) -> const float& {
        int channel = anchor * chanPerAnchor + component;
        int idx = (channel * gridH + row) * gridW + col;
        return data[idx];
    };

    out.clear();
    // Reserve a reasonable upper bound to avoid realloc.
    out.reserve(std::min(kMaxDetections, gridW * gridH * 3));

    bool capped = false;
    for (int row = 0; row < gridH && !capped; ++row) {
        for (int col = 0; col < gridW && !capped; ++col) {
            for (int anchor = 0; anchor < 3; ++anchor) {
                // Objectness logit -> objectness probability.
                float objLogit = at(anchor, 4, row, col);
                float obj = outputAlreadySigmoided ? objLogit : sigmoid(objLogit);
                if (!isFiniteSafe(obj)) {
                    err = "non-finite objectness after sigmoid";
                    return false;
                }

                // Find the best class for this anchor cell.
                int   bestClass = -1;
                float bestConf  = 0.0f;
                for (int c = 0; c < classCount; ++c) {
                    float clsLogit = at(anchor, 5 + c, row, col);
                    float clsProb  = outputAlreadySigmoided ? clsLogit : sigmoid(clsLogit);
                    if (!isFiniteSafe(clsProb)) {
                        err = "non-finite class probability";
                        return false;
                    }
                    float conf = obj * clsProb;
                    if (conf > bestConf) {
                        bestConf  = conf;
                        bestClass = c;
                    }
                }

                if (bestClass < 0) continue;
                if (bestConf < confThreshold) continue;

                // Cap candidate count (jilu.txt 鎼俆DD item 12).
                if (static_cast<int>(out.size()) >= kMaxDetections) {
                    capped = true;
                    break;
                }

                // Decode box (YOLOv5 v6, grid sensitivity 2.0).
                float tx = at(anchor, 0, row, col);
                float ty = at(anchor, 1, row, col);
                float tw = at(anchor, 2, row, col);
                float th = at(anchor, 3, row, col);

                float sx = outputAlreadySigmoided ? tx : sigmoid(tx);
                float sy = outputAlreadySigmoided ? ty : sigmoid(ty);
                float sw = outputAlreadySigmoided ? tw : sigmoid(tw);
                float sh = outputAlreadySigmoided ? th : sigmoid(th);

                float anchorW = static_cast<float>(anchors[anchor * 2 + 0]);
                float anchorH = static_cast<float>(anchors[anchor * 2 + 1]);

                float cx = (sx * 2.0f - 0.5f + static_cast<float>(col)) *
                           static_cast<float>(stride);
                float cy = (sy * 2.0f - 0.5f + static_cast<float>(row)) *
                           static_cast<float>(stride);
                float w  = std::pow(sw * 2.0f, 2.0f) * anchorW;
                float h  = std::pow(sh * 2.0f, 2.0f) * anchorH;

                if (!isFiniteSafe(cx) || !isFiniteSafe(cy) ||
                    !isFiniteSafe(w)  || !isFiniteSafe(h)) {
                    err = "non-finite box coordinate";
                    return false;
                }

                Detection d;
                d.box.cx = cx;
                d.box.cy = cy;
                d.box.w  = w;
                d.box.h  = h;
                d.confidence  = bestConf;
                d.classIndex  = bestClass;
                // className is left empty here; the CLI tool will fill it
                // from the labels file once COCO names are loaded.
                out.push_back(std::move(d));
            }
        }
    }
    return true;
}

// ============================================================
// IoU + class-aware NMS (jilu.txt 鎼俆DD item 10-11)
// ============================================================
float iou(const Box &a, const Box &b)
{
    // Convert center-format to corner-format.
    float ax1 = a.cx - a.w * 0.5f;
    float ay1 = a.cy - a.h * 0.5f;
    float ax2 = a.cx + a.w * 0.5f;
    float ay2 = a.cy + a.h * 0.5f;
    float bx1 = b.cx - b.w * 0.5f;
    float by1 = b.cy - b.h * 0.5f;
    float bx2 = b.cx + b.w * 0.5f;
    float by2 = b.cy + b.h * 0.5f;

    float areaA = a.w * a.h;
    float areaB = b.w * b.h;
    if (areaA <= 0.0f || areaB <= 0.0f) return 0.0f;

    float interX1 = std::max(ax1, bx1);
    float interY1 = std::max(ay1, by1);
    float interX2 = std::min(ax2, bx2);
    float interY2 = std::min(ay2, by2);
    float interW = interX2 - interX1;
    float interH = interY2 - interY1;
    if (interW <= 0.0f || interH <= 0.0f) return 0.0f;

    float inter = interW * interH;
    float uni   = areaA + areaB - inter;
    if (uni <= 0.0f) return 0.0f;
    return inter / uni;
}

std::vector<Detection> classAwareNms(const std::vector<Detection> &dets,
                                     float iouThreshold)
{
    // Group by classIndex so different classes never suppress each other
    // (jilu.txt 鎼俆DD item 11).
    std::vector<Detection> result;
    if (dets.empty()) return result;

    // Find max class index to size the bucket array.
    int maxClass = -1;
    for (const auto &d : dets) {
        if (d.classIndex > maxClass) maxClass = d.classIndex;
    }
    if (maxClass < 0) return result;

    std::vector<std::vector<Detection>> buckets(maxClass + 1);
    for (const auto &d : dets) {
        if (d.classIndex < 0) continue;
        buckets[d.classIndex].push_back(d);
    }

    for (auto &bucket : buckets) {
        if (bucket.empty()) continue;
        // Sort by confidence descending.
        std::sort(bucket.begin(), bucket.end(),
                  [](const Detection &x, const Detection &y) {
                      return x.confidence > y.confidence;
                  });
        std::vector<char> suppressed(bucket.size(), 0);
        for (size_t i = 0; i < bucket.size(); ++i) {
            if (suppressed[i]) continue;
            result.push_back(bucket[i]);
            if (static_cast<int>(result.size()) >= kMaxFinalDetections) {
                return result;
            }
            for (size_t j = i + 1; j < bucket.size(); ++j) {
                if (suppressed[j]) continue;
                float ov = iou(bucket[i].box, bucket[j].box);
                if (ov > iouThreshold) suppressed[j] = 1;
            }
        }
    }
    return result;
}


// ============================================================
// Multi-output production adapter
// ============================================================

namespace {

// Prepared per-layer decode context. `view` is a non-owning pointer into the
// caller's `outputs` vector (stable for the duration of the call).
// `anchors` is std::array (not C array) so it can be value-captured by
// decodeOutputsParallel's per-layer lambda under C++17.
struct PreparedLayer {
    YoloLayerInfo info;
    std::array<int, 6> anchors{};
    const OutputTensorView *view = nullptr;
};

// Shared contract validation + per-layer preparation, used by both the serial
// decodeOutputs() and the parallel decodeOutputsParallel(). Extracting this
// guarantees the two paths accept exactly the same inputs.
//
// On success `out` is sized to outputs.size() and ordered identically to
// `outputs`. On failure `error` is set and `out` is cleared.
bool prepareDecodeLayers(const std::vector<OutputTensorView> &outputs,
                         const Metadata &metadata,
                         const LetterboxParams &letterbox,
                         float confidenceThreshold,
                         float nmsThreshold,
                         std::vector<PreparedLayer> &out,
                         std::string &error)
{
    out.clear();
    error.clear();

    if (!std::isfinite(confidenceThreshold) ||
        confidenceThreshold < 0.0f || confidenceThreshold > 1.0f ||
        !std::isfinite(nmsThreshold) ||
        nmsThreshold < 0.0f || nmsThreshold > 1.0f) {
        error = "decode thresholds must be finite values in [0,1]";
        return false;
    }
    if (metadata.classCount != kCocoClassCount) {
        error = "metadata class_count must be " +
                std::to_string(kCocoClassCount);
        return false;
    }
    if (metadata.inputWidth != 640 || metadata.inputHeight != 640 ||
        metadata.inputChannels != 3 || metadata.inputLayout != "NHWC" ||
        metadata.inputType != "FLOAT16") {
        error = "metadata input_width/input_height/input_channels/input_layout/input_type contract is invalid";
        return false;
    }
    if (metadata.callerInputFormat != "NHWC") {
        error = "metadata caller_input_fmt must be NHWC";
        return false;
    }
    if (metadata.callerInputType != "UINT8") {
        error = "metadata caller_input_type must be UINT8";
        return false;
    }
    if (metadata.callerInputPassThrough != 0) {
        error = "metadata caller_input_pass_through must be 0";
        return false;
    }
    if (metadata.decodeVariant != "yolov5_v6") {
        error = "metadata decode_variant must be yolov5_v6";
        return false;
    }
    if (metadata.outputLayout != "NCHW") {
        error = "metadata output_layout must be NCHW";
        return false;
    }
    if (metadata.outputType != "FLOAT16") {
        error = "metadata output_type must be FLOAT16";
        return false;
    }
    if (metadata.outputRequest != "FLOAT32") {
        error = "metadata output_request must be FLOAT32";
        return false;
    }
    const std::vector<int> expectedDims[3] = {
        {1, 255, 80, 80}, {1, 255, 40, 40}, {1, 255, 20, 20}};
    if (metadata.outputCount != 3 || metadata.channelCount != kChannelCount ||
        metadata.outputDims[0] != expectedDims[0] ||
        metadata.outputDims[1] != expectedDims[1] ||
        metadata.outputDims[2] != expectedDims[2]) {
        error = "metadata output_count/channel_count/dims contract is invalid";
        return false;
    }
    if (!metadata.outputSigmoidBuiltin) {
        error = "metadata output_sigmoid must be builtin";
        return false;
    }
    if (std::fabs(metadata.gridSensitivity - 2.0f) > 1e-6f ||
        metadata.letterboxFill != 114) {
        error = "metadata grid_sensitivity or letterbox_fill is invalid";
        return false;
    }
    if (!std::isfinite(letterbox.scale) || letterbox.scale <= 0.0f) {
        error = "letterbox scale must be finite and positive";
        return false;
    }
    if (letterbox.srcW <= 0 || letterbox.srcH <= 0 ||
        letterbox.dstW <= 0 || letterbox.dstH <= 0) {
        error = "letterbox source and destination dimensions must be positive";
        return false;
    }
    if (letterbox.dstW != metadata.inputWidth ||
        letterbox.dstH != metadata.inputHeight) {
        error = "letterbox destination dimensions must match metadata input";
        return false;
    }
    if (letterbox.padLeft < 0 || letterbox.padRight < 0 ||
        letterbox.padTop < 0 || letterbox.padBottom < 0) {
        error = "letterbox padding must be nonnegative";
        return false;
    }
    const std::int64_t contentWidth =
        static_cast<std::int64_t>(letterbox.dstW) - letterbox.padLeft -
        letterbox.padRight;
    const std::int64_t contentHeight =
        static_cast<std::int64_t>(letterbox.dstH) - letterbox.padTop -
        letterbox.padBottom;
    if (contentWidth <= 0 || contentHeight <= 0) {
        error = "letterbox padding leaves no image content";
        return false;
    }
    const double scaledWidth = static_cast<double>(letterbox.srcW) *
                               static_cast<double>(letterbox.scale);
    const double scaledHeight = static_cast<double>(letterbox.srcH) *
                                static_cast<double>(letterbox.scale);
    if (!std::isfinite(scaledWidth) || !std::isfinite(scaledHeight) ||
        std::fabs(scaledWidth - static_cast<double>(contentWidth)) > 0.5 ||
        std::fabs(scaledHeight - static_cast<double>(contentHeight)) > 0.5) {
        error = "letterbox scale and padding are inconsistent";
        return false;
    }
    if (metadata.strides.size() != 3) {
        error = "metadata must configure exactly three output strides";
        return false;
    }

    bool configured[3] = {false, false, false};
    const auto strideSlot = [](int stride) {
        if (stride == 8) return 0;
        if (stride == 16) return 1;
        if (stride == 32) return 2;
        return -1;
    };
    for (int stride : metadata.strides) {
        const int slot = strideSlot(stride);
        if (slot < 0) {
            error = "metadata contains unsupported stride " +
                    std::to_string(stride);
            return false;
        }
        if (configured[slot]) {
            error = "metadata contains duplicate stride " +
                    std::to_string(stride);
            return false;
        }
        configured[slot] = true;
    }

    const std::vector<int> *anchorSets[3] = {
        &metadata.anchorsStride8,
        &metadata.anchorsStride16,
        &metadata.anchorsStride32
    };
    for (const std::vector<int> *anchors : anchorSets) {
        if (anchors->size() != 6 ||
            std::any_of(anchors->begin(), anchors->end(),
                        [](int value) { return value <= 0; })) {
            error = "metadata anchors must contain six positive values per stride";
            return false;
        }
    }

    if (outputs.size() != metadata.strides.size()) {
        error = "missing output layer: output count does not match metadata strides";
        return false;
    }

    bool seen[3] = {false, false, false};
    out.reserve(outputs.size());
    for (const OutputTensorView &view : outputs) {
        const int stride = strideFromGrid(view.gridWidth, view.gridHeight);
        const int slot = strideSlot(stride);
        if (slot < 0 || !configured[slot]) {
            error = "unknown or unconfigured output layer grid";
            out.clear();
            return false;
        }
        if (seen[slot]) {
            error = "duplicate output stride " + std::to_string(stride);
            out.clear();
            return false;
        }
        seen[slot] = true;

        const std::size_t elementCount =
            static_cast<std::size_t>(view.gridWidth) *
            static_cast<std::size_t>(view.gridHeight) *
            static_cast<std::size_t>(kChannelCount);
        if (elementCount > static_cast<std::size_t>(
                               std::numeric_limits<int>::max())) {
            error = "output layer element count exceeds supported range";
            out.clear();
            return false;
        }

        PreparedLayer prepared;
        if (!buildYoloLayer(view.gridWidth, view.gridHeight, stride,
                            static_cast<int>(elementCount), kChannelCount,
                            view.outputIndex, prepared.info, error)) {
            out.clear();
            return false;
        }
        if (view.data == nullptr) {
            error = "output layer data is null";
            out.clear();
            return false;
        }
        if (elementCount > std::numeric_limits<std::size_t>::max() /
                               sizeof(float) ||
            view.byteSize < elementCount * sizeof(float)) {
            error = "output layer buffer is undersized";
            out.clear();
            return false;
        }

        std::copy(anchorSets[slot]->begin(), anchorSets[slot]->end(),
                  prepared.anchors.begin());
        prepared.view = &view;
        out.push_back(std::move(prepared));
    }

    for (int slot = 0; slot < 3; ++slot) {
        if (configured[slot] && !seen[slot]) {
            error = "missing configured output layer";
            out.clear();
            return false;
        }
    }
    return true;
}

// Shared post-decode step: class-aware NMS + reverseLetterbox + finiteness
// check. Used by both serial and parallel paths so their output is identical.
bool finalizeDetections(std::vector<Detection> &candidates,
                        const LetterboxParams &letterbox,
                        float nmsThreshold,
                        std::vector<Detection> &detections,
                        std::string &error)
{
    detections.clear();
    error.clear();
    std::vector<Detection> retained = classAwareNms(candidates, nmsThreshold);
    for (Detection &detection : retained) {
        detection.box = reverseLetterbox(detection.box, letterbox);
        if (!std::isfinite(detection.box.cx) ||
            !std::isfinite(detection.box.cy) ||
            !std::isfinite(detection.box.w) ||
            !std::isfinite(detection.box.h) ||
            !std::isfinite(detection.confidence)) {
            error = "non-finite final detection";
            return false;
        }
    }
    detections.swap(retained);
    return true;
}

} // namespace

bool decodeOutputs(const std::vector<OutputTensorView> &outputs,
                   const Metadata &metadata,
                   const LetterboxParams &letterbox,
                   float confidenceThreshold,
                   float nmsThreshold,
                   std::vector<Detection> &detections,
                   std::string &error)
{
    detections.clear();

    std::vector<PreparedLayer> prepared;
    if (!prepareDecodeLayers(outputs, metadata, letterbox,
                             confidenceThreshold, nmsThreshold, prepared, error))
        return false;

    std::vector<Detection> candidates;
    for (const PreparedLayer &p : prepared) {
        std::vector<Detection> layerDetections;
        if (!decodeYoloLayer(p.view->data, p.info, p.anchors.data(),
                             metadata.classCount, confidenceThreshold,
                             metadata.outputSigmoidBuiltin,
                             layerDetections, error))
            return false;
        candidates.insert(candidates.end(),
                          std::make_move_iterator(layerDetections.begin()),
                          std::make_move_iterator(layerDetections.end()));
    }

    return finalizeDetections(candidates, letterbox, nmsThreshold,
                              detections, error);
}

bool decodeOutputsParallel(const std::vector<OutputTensorView> &outputs,
                           const Metadata &metadata,
                           const LetterboxParams &letterbox,
                           float confidenceThreshold,
                           float nmsThreshold,
                           std::vector<Detection> &detections,
                           std::string &error,
                           concurrency::ThreadPool &pool)
{
    detections.clear();

    std::vector<PreparedLayer> prepared;
    if (!prepareDecodeLayers(outputs, metadata, letterbox,
                             confidenceThreshold, nmsThreshold, prepared, error))
        return false;

    // Per-layer results + error slot. Each task writes its own slot; the main
    // thread joins all futures before touching them.
    //
    // NOTE: perLayerOk uses std::vector<char> instead of std::vector<bool>
    // because std::vector<bool> is a space-optimized specialization whose
    // operator[] returns a proxy (std::vector<bool>::reference), not a real
    // bool&. We need to capture a stable bool& by reference in the per-layer
    // lambda, so char (1 byte, addressable) is used as a stand-in.
    const std::size_t layerCount = prepared.size();
    std::vector<std::vector<Detection>> perLayer(layerCount);
    std::vector<std::string> perLayerError(layerCount);
    std::vector<char> perLayerOk(layerCount, 0);

    std::vector<std::future<void>> futures;
    futures.reserve(layerCount);
    for (std::size_t i = 0; i < layerCount; ++i) {
        PreparedLayer &p = prepared[i];
        // Capture by value: p.info (POD), p.anchors (std::array, value-capturable
        // under C++17), p.view (raw pointer into the caller's stable `outputs`
        // vector). The lambda writes only into this slot's
        // perLayer[i]/perLayerError[i]/perLayerOk[i] — no shared write.
        auto task = [info = p.info,
                     anchors = p.anchors,
                     view = p.view,
                     classCount = metadata.classCount,
                     outputAlreadySigmoided = metadata.outputSigmoidBuiltin,
                     confThreshold = confidenceThreshold,
                     &outDets = perLayer[i],
                     &outErr = perLayerError[i],
                     &outOk = perLayerOk[i]]() {
            std::vector<Detection> local;
            std::string localErr;
            const bool ok = decodeYoloLayer(view->data, info, anchors.data(),
                                            classCount, confThreshold,
                                            outputAlreadySigmoided,
                                            local, localErr);
            if (ok) {
                outDets.swap(local);
                outErr.clear();
                outOk = 1;
            } else {
                outErr.swap(localErr);
                outOk = 0;
            }
        };
        futures.push_back(pool.submit(std::move(task)));
    }

    // Await all layers. future::get() rethrows if the task threw; we translate
    // that into a generic decode failure so the public contract never leaks
    // an exception out of decodeOutputsParallel().
    for (std::size_t i = 0; i < layerCount; ++i) {
        try {
            futures[i].get();
        } catch (const std::exception &e) {
            error = "decode layer " + std::to_string(i) +
                    " threw: " + e.what();
            return false;
        }
    }

    // Merge in stride order (8, 16, 32 as discovered) — order matters only
    // for deterministic test output, not for NMS correctness (NMS sorts by
    // confidence). Use the first failing layer's error for determinism.
    std::vector<Detection> candidates;
    for (std::size_t i = 0; i < layerCount; ++i) {
        if (perLayerOk[i] == 0) {
            error = perLayerError[i];
            return false;
        }
        candidates.insert(candidates.end(),
                          std::make_move_iterator(perLayer[i].begin()),
                          std::make_move_iterator(perLayer[i].end()));
    }

    return finalizeDetections(candidates, letterbox, nmsThreshold,
                              detections, error);
}

// ============================================================
// Expect-class validation (jilu.txt 鎼俆DD item 14)
// ============================================================
bool findExpectedClass(const std::vector<Detection> &dets, int expectedClassIndex)
{
    if (expectedClassIndex < 0) return false;
    for (const auto &d : dets) {
        if (d.classIndex == expectedClassIndex) return true;
    }
    return false;
}

} // namespace rknn_postprocess
