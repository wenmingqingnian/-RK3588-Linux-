// test_rknn_image_common — pure-logic unit tests for Stage B helpers.
//
// Implements jilu.txt §5 "先写纯逻辑测试" — 14 required coverage items.
// Host-compilable (g++/clang++ C++17) without any NPU/Qt/RKNN dependency.
//
// Usage:
//   g++ -std=c++17 -Wall -Wextra -Wpedantic
//       test_rknn_image_common.cpp ../../media/rknn_postprocess.cpp
//       -o test_rknn_image_common
//   ./test_rknn_image_common
//
// Exit code: 0 = all pass, 1 = at least one failure.

#include "../../media/rknn_postprocess.h"

namespace rknn_image = rknn_postprocess;
// Compatibility alias keeps the verified test body unchanged after promotion.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;

// Generic value-to-string helper so CHECK_EQ can print any comparable type
// (int, size_t, float, bool, std::string, const char*).
template <typename T>
inline std::string toDebugString(const T &v) { return std::to_string(v); }
inline std::string toDebugString(const std::string &s) { return s; }
inline std::string toDebugString(const char *s) { return s ? s : "(null)"; }
inline std::string toDebugString(bool b) { return b ? "true" : "false"; }

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (cond) {                                                          \
            ++g_pass;                                                        \
        } else {                                                             \
            ++g_fail;                                                        \
            std::printf("FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg);       \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b, msg)                                                  \
    do {                                                                     \
        auto _va = (a);                                                      \
        auto _vb = (b);                                                      \
        if (_va == _vb) {                                                    \
            ++g_pass;                                                        \
        } else {                                                             \
            ++g_fail;                                                        \
            std::printf("FAIL [%s:%d] %s (got=%s want=%s)\n",                \
                        __FILE__, __LINE__, msg,                             \
                        toDebugString(_va).c_str(),                          \
                        toDebugString(_vb).c_str());                         \
        }                                                                    \
    } while (0)

using rknn_image::Box;
using rknn_image::buildYoloLayer;
using rknn_image::classAwareNms;
using rknn_image::computeLetterbox;
using rknn_image::decodeYoloLayer;
using rknn_image::Detection;
using rknn_image::findExpectedClass;
using rknn_image::findClassIndexByName;
using rknn_image::iou;
using rknn_image::kCocoClassCount;
using rknn_image::kChannelCount;
using rknn_image::kLetterboxFill;
using rknn_image::kMaxDetections;
using rknn_image::kMaxFinalDetections;
using rknn_image::LetterboxParams;
using rknn_image::loadLabels;
using rknn_image::Metadata;
using rknn_image::parseMetadata;
using rknn_image::reverseLetterbox;
using rknn_image::sigmoid;
using rknn_image::strideFromGrid;
using rknn_image::validateMetadata;
using rknn_image::validateAndNormalizeBox;
using rknn_image::YoloLayerInfo;

// ---------- helpers for synthetic metadata ----------
Metadata makeValidMetadata(const std::string &sha)
{
    Metadata m;
    m.modelSha256    = sha;
    m.onnxSha256     = "edae4dbd2b2e1f4d5c3c5a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a";
    m.provenance     = "yolov5s.onnx via convert_onnx_to_rknn.py";
    m.labelsFile     = "coco_80_labels.txt";
    m.decodeVariant  = "yolov5_v6";
    m.inputWidth     = 640;
    m.inputHeight    = 640;
    m.inputChannels  = 3;
    m.inputLayout    = "NHWC";
    m.inputType      = "FLOAT16";
    m.callerInputFormat = "NHWC";
    m.callerInputType = "UINT8";
    m.callerInputPassThrough = 0;
    m.classCount     = kCocoClassCount;
    m.channelCount   = kChannelCount;
    m.outputCount    = 3;
    m.outputLayout   = "NCHW";
    m.outputType     = "FLOAT16";
    m.outputRequest  = "FLOAT32";
    m.outputDims[0]  = {1, 255, 80, 80};
    m.outputDims[1]  = {1, 255, 40, 40};
    m.outputDims[2]  = {1, 255, 20, 20};
    m.outputSigmoidBuiltin = true;
    m.gridSensitivity = 2.0f;
    m.letterboxFill = 114;
    m.strides        = {8, 16, 32};
    m.anchorsStride8  = {10, 13, 16, 30, 33, 23};
    m.anchorsStride16 = {30, 61, 62, 45, 59, 119};
    m.anchorsStride32 = {116, 90, 156, 198, 373, 326};
    m.goldenImage     = "bus.jpg";
    m.goldenImageSha256 =
        "33b198a1d2839bb9ac4c65d61f9e852196793cae9a0781360859425f6022b69c";
    m.expectedClass      = "bus";
    m.expectedClassIndex = 5;
    return m;
}

// ============================================================
// TDD item 1: metadata SHA match / mismatch (jilu.txt §5.1)
// ============================================================
void testMetadataShaMatchMismatch()
{
    std::string err;
    Metadata m = makeValidMetadata("AAAA");

    // Mismatch -> false
    CHECK(!validateMetadata(m, "BBBB", err), "SHA mismatch should reject");
    CHECK(!err.empty(), "error message should be non-empty on mismatch");

    // Match -> true
    err.clear();
    CHECK(validateMetadata(m, "AAAA", err), "SHA match should accept");
    CHECK(err.empty(), "error message should be empty on match");
}

void testMetadataCallerInputContract()
{
    std::string err;
    Metadata m = makeValidMetadata("AAAA");
    CHECK(validateMetadata(m, "AAAA", err), "NHWC UINT8 caller input should pass");
    m.callerInputFormat = "NCHW";
    CHECK(!validateMetadata(m, "AAAA", err), "non-NHWC caller input should reject");
    m = makeValidMetadata("AAAA");
    m.callerInputType = "FLOAT16";
    CHECK(!validateMetadata(m, "AAAA", err), "non-UINT8 caller input should reject");
    m = makeValidMetadata("AAAA");
    m.callerInputPassThrough = 1;
    CHECK(!validateMetadata(m, "AAAA", err), "caller pass-through must reject");
    m = makeValidMetadata("AAAA");
    m.outputSigmoidBuiltin = false;
    CHECK(!validateMetadata(m, "AAAA", err), "non-builtin sigmoid must reject");
    m = makeValidMetadata("AAAA");
    m.inputLayout = "NCHW";
    CHECK(!validateMetadata(m, "AAAA", err), "wrong native input layout must reject");
}

void testNormalizeBoundaryBoxes()
{
    std::string err;
    Box normalized{};
    CHECK(validateAndNormalizeBox(Box{320.0f, 320.0f, 640.0f, 640.0f},
                                  640, 640, normalized, err),
          "exact full-image boundary box should pass");
    CHECK(std::fabs(normalized.cx - 0.5f) < 1e-6f, "exact cx normalized");
    CHECK(std::fabs(normalized.w - 1.0f) < 1e-6f, "exact width normalized");

    const float fewUlps = std::nextafter(std::nextafter(320.0f,
        std::numeric_limits<float>::infinity()),
        std::numeric_limits<float>::infinity());
    CHECK(validateAndNormalizeBox(Box{fewUlps, 320.0f, 640.0f, 640.0f},
                                  640, 640, normalized, err),
          "few-ULP edge overflow should pass");
    CHECK(normalized.cx >= 0.0f && normalized.cx <= 1.0f &&
          normalized.w >= 0.0f && normalized.w <= 1.0f,
          "few-ULP box should clamp normalized values");
    CHECK(!validateAndNormalizeBox(Box{321.0f, 320.0f, 640.0f, 640.0f},
                                   640, 640, normalized, err),
          "real edge overflow should reject");
    CHECK(!validateAndNormalizeBox(Box{10.0f, 10.0f, 0.0f, 4.0f},
                                   640, 640, normalized, err),
          "zero-area box should reject");
}

// ============================================================
// TDD item 2: class_count must be 80 (jilu.txt §5.2)
// ============================================================
void testMetadataClassCountValidation()
{
    std::string err;
    Metadata m = makeValidMetadata("AAAA");

    // classCount == 80 -> ok
    CHECK(validateMetadata(m, "AAAA", err), "classCount=80 should pass");

    // classCount == 79 -> reject
    m.classCount = 79;
    CHECK(!validateMetadata(m, "AAAA", err), "classCount=79 should reject");

    // classCount == 81 -> reject
    m.classCount = 81;
    CHECK(!validateMetadata(m, "AAAA", err), "classCount=81 should reject");

    // classCount == 0 -> reject
    m.classCount = 0;
    CHECK(!validateMetadata(m, "AAAA", err), "classCount=0 should reject");

    // classCount < 0 -> reject
    m.classCount = -1;
    CHECK(!validateMetadata(m, "AAAA", err), "classCount<0 should reject");
}

// ============================================================
// TDD item 3: channel must be 255 (jilu.txt §5.3)
// Validates buildYoloLayer with channelCount != 255 rejects.
// ============================================================
void testChannelCount255()
{
    std::string err;
    YoloLayerInfo info;

    // Valid: channelCount=255, gridW=80, gridH=80, stride=8
    int nElems = 80 * 80 * kChannelCount;
    CHECK(buildYoloLayer(80, 80, 8, nElems, kChannelCount, 0, info, err),
          "channelCount=255 stride=8 should succeed");

    // Wrong channelCount -> reject
    CHECK(!buildYoloLayer(80, 80, 8, 80 * 80 * 84, 84, 0, info, err),
          "channelCount=84 should reject");

    // nElems mismatch -> reject
    CHECK(!buildYoloLayer(80, 80, 8, 80 * 80 * kChannelCount + 1,
                          kChannelCount, 0, info, err),
          "nElems mismatch should reject");

    // This product is larger than INT_MAX but wraps to 1844840192 with
    // unchecked 32-bit signed arithmetic. It must be rejected safely.
    CHECK(!buildYoloLayer(50000, 50000, 8, 1844840192,
                          kChannelCount, 0, info, err),
          "overflowing layer element count should reject");
    CHECK(!err.empty(), "overflow rejection should explain the error");
}

// ============================================================
// TDD item 4: NCHW channel-major indexing (jilu.txt §5.4, line 397-402)
//
//   channel = anchor * (5 + classCount) + component
//   index   = (channel * gridH + row) * gridW + col
//
// Construct a tiny synthetic layer (gridW=2, gridH=2, stride=1, anchors
// {1,1, 1,1, 1,1}) and place a strong signal at a known index, then verify
// decodeYoloLayer finds it.
// ============================================================
void testNchwChannelMajorIndex()
{
    const int gridW = 2, gridH = 2, stride = 1;
    const int channelCount = 3 * (5 + kCocoClassCount); // 255
    const int nElems = gridW * gridH * channelCount;

    std::vector<float> data(nElems, 0.0f);

    // Pick a target: anchor=1, component=4 (objectness logit), row=1, col=0.
    // Pre-sigmoid value = large positive -> sigmoid ≈ 1.0
    // Place class=0 logit also large, so confidence ≈ 1.0.
    const int anchor   = 1;
    const int classIdx = 0;
    const int row = 1, col = 0;

    const int objChannel = anchor * (5 + kCocoClassCount) + 4;
    const int clsChannel = anchor * (5 + kCocoClassCount) + 5 + classIdx;
    const int cxChannel  = anchor * (5 + kCocoClassCount) + 0;
    const int cyChannel  = anchor * (5 + kCocoClassCount) + 1;
    const int wChannel   = anchor * (5 + kCocoClassCount) + 2;
    const int hChannel   = anchor * (5 + kCocoClassCount) + 3;

    auto idx = [&](int channel, int r, int c) {
        return (channel * gridH + r) * gridW + c;
    };

    // Make box reasonable: sigmoid(0)=0.5; cx=(0.5*2-0.5+0)*1=0.5
    // w=pow(sigmoid(0)*2,2)*1=1.0
    data[idx(objChannel, row, col)] = 10.0f;  // sigmoid ≈ 1.0
    data[idx(clsChannel, row, col)] = 10.0f;  // class 0 conf ≈ 1.0
    data[idx(cxChannel,  row, col)] = 0.0f;   // cx = 0.5*stride
    data[idx(cyChannel,  row, col)] = 0.0f;   // cy = 0.5*stride
    data[idx(wChannel,   row, col)] = 0.0f;   // w = 1.0*anchorW = 1.0
    data[idx(hChannel,   row, col)] = 0.0f;   // h = 1.0*anchorH = 1.0

    YoloLayerInfo info;
    std::string err;
    CHECK(buildYoloLayer(gridW, gridH, stride, nElems, channelCount, 0, info, err),
          "buildYoloLayer tiny grid should succeed");

    int anchors[6] = {1, 1, 1, 1, 1, 1};
    std::vector<Detection> dets;
    bool ok = decodeYoloLayer(data.data(), info, anchors, kCocoClassCount,
                              0.5f, false, dets, err);
    CHECK(ok, "decodeYoloLayer should succeed on synthetic data");

    // Expect exactly one detection at anchor=1, class=0
    CHECK_EQ(dets.size(), static_cast<size_t>(1), "exactly one detection");
    if (!dets.empty()) {
        CHECK_EQ(dets[0].classIndex, 0, "class index 0");
        CHECK(dets[0].confidence >= 0.5f, "confidence above threshold");
        // cx = (sigmoid(0)*2-0.5+0)*1 = 0.5
        CHECK(std::fabs(dets[0].box.cx - 0.5f) < 1e-5f, "cx=0.5");
        // cy = (sigmoid(0)*2-0.5+1)*1 = 1.5
        CHECK(std::fabs(dets[0].box.cy - 1.5f) < 1e-5f, "cy=1.5");
        // w = pow(sigmoid(0)*2, 2) * 1 = 1.0
        CHECK(std::fabs(dets[0].box.w - 1.0f) < 1e-5f, "w=1.0");
        CHECK(std::fabs(dets[0].box.h - 1.0f) < 1e-5f, "h=1.0");
    }
}

// ============================================================
// TDD item 5: three grids <-> stride mapping (jilu.txt §5.5)
//   80x80 -> stride 8
//   40x40 -> stride 16
//   20x20 -> stride 32
// ============================================================
void testStrideFromGrid()
{
    CHECK_EQ(strideFromGrid(80, 80), 8,  "80x80 -> stride 8");
    CHECK_EQ(strideFromGrid(40, 40), 16, "40x40 -> stride 16");
    CHECK_EQ(strideFromGrid(20, 20), 32, "20x20 -> stride 32");

    // Non-square grids at known YOLO sizes should still map (larger dim drives).
    CHECK_EQ(strideFromGrid(80, 40), 0,  "non-matching grid -> 0");
    CHECK_EQ(strideFromGrid(41, 41), 0,  "unknown grid -> 0");
    CHECK_EQ(strideFromGrid(0, 0),   0,  "zero grid -> 0");
    CHECK_EQ(strideFromGrid(160, 160), 0, "160 grid -> 0 (not a YOLOv5 grid)");
}

// ============================================================
// TDD item 6: letterbox even/odd padding (jilu.txt §5.6)
// ============================================================
void testLetterboxPadding()
{
    // Even case: 1920x1080 -> 640x640
    //   scale = min(640/1920, 640/1080) = 0.333... (use 1920)
    //   newW = round(1920*0.333...) = 640
    //   newH = round(1080*0.333...) = 360
    //   padLeft = padRight = 0
    //   padTop = padBottom = (640-360)/2 = 140  (even)
    {
        LetterboxParams p = computeLetterbox(1920, 1080, 640, 640);
        CHECK(p.scale > 0.0f, "scale > 0");
        CHECK_EQ(p.padLeft + p.padRight, 0, "no horizontal pad for 1920->640");
        CHECK_EQ(p.padTop + p.padBottom, 640 - 360, "vertical pad = 280");
        CHECK_EQ(p.padTop, p.padBottom, "even padTop==padBottom");
        CHECK_EQ(p.padTop % 2, 0, "even padding");
    }

    // Odd case: 1001x1001 -> 64x64
    //   scale = 64/1001 = 0.063936...
    //   newW = newH = round(1001*0.063936) = 64
    //   total pad = 0 — pick a case that yields odd padding instead.
    //
    // Use 100x50 -> 32x32:
    //   scale = min(32/100, 32/50) = 0.32
    //   newW = round(100*0.32) = 32
    //   newH = round(50*0.32)  = 16
    //   padTop+padBottom = 32-16 = 16 (even). Need odd.
    //
    // Use 11x11 -> 4x4:
    //   scale = min(4/11, 4/11) = 0.363636...
    //   newW = round(11*0.3636) = 4
    //   newH = 4
    //   pad = 0
    //
    // Use 11x5 -> 4x4:
    //   scale = min(4/11, 4/5) = 4/11 = 0.3636...
    //   newW = round(11 * 4/11) = 4
    //   newH = round(5 * 4/11) = round(1.818) = 2
    //   padTop+padBottom = 4-2 = 2 (even)
    //
    // Use 13x7 -> 4x4:
    //   scale = min(4/13, 4/7) = 4/13 ≈ 0.3076923
    //   newW = round(13*0.3076923) = 4
    //   newH = round(7*0.3076923) = round(2.1538) = 2
    //   padTop+padBottom = 2 (even). Need odd.
    //
    // Use 9x3 -> 4x4:
    //   scale = min(4/9, 4/3) = 4/9 = 0.4444
    //   newW = round(9*0.4444) = 4
    //   newH = round(3*0.4444) = round(1.333) = 1
    //   padTop + padBottom = 3 (odd!) -> one of padTop/padBottom will be 2, other 1
    {
        LetterboxParams p = computeLetterbox(9, 3, 4, 4);
        CHECK(p.scale > 0.0f, "odd-case scale > 0");
        int vsum = p.padTop + p.padBottom;
        CHECK_EQ(vsum, 4 - 1, "vertical pad total = 3 (odd)");
        // For odd padding, padTop = ceil(3/2) = 2, padBottom = 1 (or similar split)
        CHECK(std::abs(p.padTop - p.padBottom) <= 1,
              "odd padding splits within 1");
        CHECK_EQ(p.padLeft + p.padRight, 0, "no horizontal pad");
    }

    // Already exact fit: 640x640 -> 640x640
    {
        LetterboxParams p = computeLetterbox(640, 640, 640, 640);
        CHECK(std::fabs(p.scale - 1.0f) < 1e-6f, "scale=1.0 for exact fit");
        CHECK_EQ(p.padLeft + p.padRight + p.padTop + p.padBottom, 0,
                 "no padding for exact fit");
    }
}

// ============================================================
// TDD item 7: coordinate reverse transform (jilu.txt §5.7)
// ============================================================
void testReverseLetterboxTransform()
{
    // 1920x1080 -> 640x640 with scale=1/3, newH=360, padTop=padBottom=140
    LetterboxParams p = computeLetterbox(1920, 1080, 640, 640);

    // A box centered at model coords (320, 320) — i.e. center of the
    // letterboxed region — should map back to original center (960, 540).
    Box modelBox{320.0f, 320.0f, 100.0f, 50.0f};
    Box orig = reverseLetterbox(modelBox, p);
    // orig.cx = (320 - padLeft) / scale = 320 / (1/3) = 960
    CHECK(std::fabs(orig.cx - 960.0f) < 1.0f, "orig.cx ≈ 960");
    // orig.cy = (320 - padTop) / scale = (320 - 140) / (1/3) = 540
    CHECK(std::fabs(orig.cy - 540.0f) < 1.0f, "orig.cy ≈ 540");
    // orig.w = 100 / scale = 300
    CHECK(std::fabs(orig.w - 300.0f) < 1.0f, "orig.w ≈ 300");
    // orig.h = 50 / scale = 150
    CHECK(std::fabs(orig.h - 150.0f) < 1.0f, "orig.h ≈ 150");
}

// ============================================================
// TDD item 8: boundary clipping (jilu.txt §5.8)
// Box outside the model grid should be clipped to source image bounds.
// ============================================================
void testReverseLetterboxBoundaryClip()
{
    LetterboxParams p = computeLetterbox(1920, 1080, 640, 640);
    // scale = 1/3, padLeft=0, padTop=140, padRight=0, padBottom=140

    // Box at model coord (-50, -50) with w=1000, h=1000 -> should clip.
    Box bad{-50.0f, -50.0f, 1000.0f, 1000.0f};
    Box orig = reverseLetterbox(bad, p);

    // After clip: cx in [0, 1920], cy in [0, 1080], w/h bounded.
    CHECK(orig.cx >= 0.0f && orig.cx <= 1920.0f, "orig.cx in [0,1920]");
    CHECK(orig.cy >= 0.0f && orig.cy <= 1080.0f, "orig.cy in [0,1080]");
    CHECK(orig.w >= 0.0f && orig.w <= 1920.0f,   "orig.w in [0,1920]");
    CHECK(orig.h >= 0.0f && orig.h <= 1080.0f,   "orig.h in [0,1080]");

    // Extreme: fully outside the original image — corner box.
    Box corner{-1000.0f, -1000.0f, 10.0f, 10.0f};
    Box oc = reverseLetterbox(corner, p);
    CHECK(oc.cx >= 0.0f && oc.cx <= 1920.0f, "corner cx clipped");
    CHECK(oc.cy >= 0.0f && oc.cy <= 1080.0f, "corner cy clipped");

    // A box fully beyond the right and bottom edges must collapse onto the
    // valid boundary, not retain an out-of-range first corner.
    Box beyond{1000.0f, 1000.0f, 10.0f, 10.0f};
    Box ob = reverseLetterbox(beyond, p);
    const float x1 = ob.cx - ob.w * 0.5f;
    const float x2 = ob.cx + ob.w * 0.5f;
    const float y1 = ob.cy - ob.h * 0.5f;
    const float y2 = ob.cy + ob.h * 0.5f;
    CHECK(x1 >= 0.0f && x1 <= 1920.0f, "beyond-right x1 clipped");
    CHECK(x2 >= 0.0f && x2 <= 1920.0f, "beyond-right x2 clipped");
    CHECK(y1 >= 0.0f && y1 <= 1080.0f, "beyond-bottom y1 clipped");
    CHECK(y2 >= 0.0f && y2 <= 1080.0f, "beyond-bottom y2 clipped");
}

// ============================================================
// TDD item 9: NaN/Inf rejection (jilu.txt §5.9)
// ============================================================
void testNanInfRejection()
{
    const int gridW = 4, gridH = 4, stride = 8;
    const int nElems = gridW * gridH * kChannelCount;
    std::vector<float> data(nElems, 0.0f);

    YoloLayerInfo info;
    std::string err;
    CHECK(buildYoloLayer(gridW, gridH, stride, nElems, kChannelCount,
                         0, info, err),
          "buildYoloLayer for NaN test");

    int anchors[6] = {10, 13, 16, 30, 33, 23};

    // Inject NaN at the objectness channel of anchor 0, row 0, col 0.
    int nanChannel = 0 * (5 + kCocoClassCount) + 4;
    int nanIdx = (nanChannel * gridH + 0) * gridW + 0;
    data[nanIdx] = std::numeric_limits<float>::quiet_NaN();
    {
        std::vector<Detection> dets;
        bool ok = decodeYoloLayer(data.data(), info, anchors, kCocoClassCount,
                                  0.5f, false, dets, err);
        CHECK(!ok, "NaN data should be rejected");
        CHECK(!err.empty(), "error message should explain NaN/Inf");
    }

    // Reset, inject Inf
    data[nanIdx] = 0.0f;
    int infIdx = (0 * (5 + kCocoClassCount) + 0) * gridH * gridW + 0;
    data[infIdx] = std::numeric_limits<float>::infinity();
    {
        std::vector<Detection> dets;
        bool ok = decodeYoloLayer(data.data(), info, anchors, kCocoClassCount,
                                  0.5f, false, dets, err);
        CHECK(!ok, "Inf data should be rejected");
    }
}

// ============================================================
// TDD item 10: same-class NMS suppression (jilu.txt §5.10)
// ============================================================
void testSameClassNms()
{
    std::vector<Detection> dets;
    // Two heavily overlapping boxes of the same class.
    Detection a; a.box = {100, 100, 80, 80}; a.confidence = 0.9f; a.classIndex = 0;
    Detection b; b.box = {105, 105, 80, 80}; b.confidence = 0.8f; b.classIndex = 0;
    dets.push_back(a);
    dets.push_back(b);

    auto out = classAwareNms(dets, 0.3f);
    CHECK_EQ(out.size(), static_cast<size_t>(1), "same-class NMS keeps 1");
    if (!out.empty()) {
        CHECK_EQ(out[0].classIndex, 0, "kept class 0");
        CHECK(std::fabs(out[0].confidence - 0.9f) < 1e-5f,
              "kept higher confidence");
    }

    // Non-overlapping same-class boxes both kept.
    Detection c; c.box = {1000, 1000, 50, 50}; c.confidence = 0.7f; c.classIndex = 0;
    dets.push_back(c);
    out = classAwareNms(dets, 0.3f);
    CHECK_EQ(out.size(), static_cast<size_t>(2), "non-overlap keeps both");
}

// ============================================================
// TDD item 11: different-class non-suppression (jilu.txt §5.11)
// ============================================================
void testDifferentClassNotSuppressed()
{
    std::vector<Detection> dets;
    Detection a; a.box = {100, 100, 80, 80}; a.confidence = 0.9f; a.classIndex = 0;
    Detection b; b.box = {100, 100, 80, 80}; b.confidence = 0.85f; b.classIndex = 1;
    dets.push_back(a);
    dets.push_back(b);

    auto out = classAwareNms(dets, 0.3f);
    CHECK_EQ(out.size(), static_cast<size_t>(2),
             "different classes both kept despite full overlap");
}

// ============================================================
// TDD item 12: max candidates & final count (jilu.txt §5.12)
// ============================================================
void testMaxDetectionsCap()
{
    // Build a grid containing more than kMaxDetections candidates above
    // threshold. Use gridW=80, gridH=80 stride=8 → 6400 cells × 3 anchors
    // = 19200 candidates. Make all of them pass the threshold.
    const int gridW = 80, gridH = 80, stride = 8;
    const int nElems = gridW * gridH * kChannelCount;
    std::vector<float> data(nElems, 0.0f);

    // For each (row, col, anchor): set objectness=10, class 0 logit=10,
    // box coords reasonable so confidence ≈ 1.0.
    for (int row = 0; row < gridH; ++row) {
        for (int col = 0; col < gridW; ++col) {
            for (int anchor = 0; anchor < 3; ++anchor) {
                int base = anchor * (5 + kCocoClassCount);
                int objCh = base + 4;
                int clsCh = base + 5;       // class 0
                // cx,cy,tw,th = 0 -> cx=(0.5*2-0.5+col)*8, w=pow(1,2)*anchorW
                int objIdx = (objCh * gridH + row) * gridW + col;
                int clsIdx = (clsCh * gridH + row) * gridW + col;
                data[objIdx] = 10.0f;
                data[clsIdx] = 10.0f;
            }
        }
    }

    YoloLayerInfo info;
    std::string err;
    CHECK(buildYoloLayer(gridW, gridH, stride, nElems, kChannelCount,
                         0, info, err),
          "buildYoloLayer for cap test");

    int anchors[6] = {10, 13, 16, 30, 33, 23};
    std::vector<Detection> dets;
    bool ok = decodeYoloLayer(data.data(), info, anchors, kCocoClassCount,
                              0.5f, false, dets, err);
    CHECK(ok, "decode should succeed");
    // Implementation must cap candidates at kMaxDetections.
    CHECK(static_cast<int>(dets.size()) <= kMaxDetections,
          "candidates capped at kMaxDetections");

    // Run NMS and verify final count is bounded.
    auto out = classAwareNms(dets, 0.45f);
    CHECK(static_cast<int>(out.size()) <= kMaxFinalDetections,
          "final detections <= kMaxFinalDetections");
}

// ============================================================
// TDD item 13: empty detection result (jilu.txt §5.13)
// ============================================================
void testEmptyDetection()
{
    const int gridW = 20, gridH = 20, stride = 32;
    const int nElems = gridW * gridH * kChannelCount;
    std::vector<float> data(nElems, 0.0f);

    // All-zero data: sigmoid(0)=0.5 for objectness, sigmoid(0)=0.5 for class
    // → confidence = 0.25 < 0.5 threshold → no detections.
    YoloLayerInfo info;
    std::string err;
    CHECK(buildYoloLayer(gridW, gridH, stride, nElems, kChannelCount,
                         0, info, err),
          "buildYoloLayer for empty test");

    int anchors[6] = {116, 90, 156, 198, 373, 326};
    std::vector<Detection> dets;
    bool ok = decodeYoloLayer(data.data(), info, anchors, kCocoClassCount,
                              0.5f, false, dets, err);
    CHECK(ok, "decode of all-zero data should succeed");
    CHECK_EQ(dets.size(), static_cast<size_t>(0), "no detections");

    // NMS on empty input -> empty output.
    auto out = classAwareNms(dets, 0.45f);
    CHECK_EQ(out.size(), static_cast<size_t>(0), "NMS of empty -> empty");
}

// ============================================================
// TDD item 14: expect-class validation (jilu.txt §5.14)
// ============================================================
void testExpectClassValidation()
{
    std::vector<Detection> dets;
    Detection d0; d0.classIndex = 0; dets.push_back(d0);
    Detection d1; d1.classIndex = 5; dets.push_back(d1);   // bus
    Detection d2; d2.classIndex = 7; dets.push_back(d2);

    CHECK(findExpectedClass(dets, 5),  "expected class 5 present");
    CHECK(!findExpectedClass(dets, 99), "expected class 99 absent");
    CHECK(findExpectedClass(dets, 0),  "expected class 0 present");

    // Empty list
    std::vector<Detection> empty;
    CHECK(!findExpectedClass(empty, 5), "empty list -> false");

    // Negative index
    CHECK(!findExpectedClass(dets, -1), "negative index -> false");
}

void testClassIndexByName()
{
    const std::vector<std::string> labels = {
        "person", "bicycle", "car", "motorcycle", "airplane", "bus"
    };
    CHECK_EQ(findClassIndexByName(labels, "bus"), 5,
             "known class name resolves to its label index");
    CHECK_EQ(findClassIndexByName(labels, "buss"), -1,
             "unknown explicit class name is rejected");
}

// ============================================================
// Bonus: loadLabels round-trip (light coverage of file path)
// ============================================================
void testLoadLabelsRoundTrip()
{
    // Use current-directory relative paths for cross-platform compatibility
    // (Windows has no /tmp; Linux tests typically run from a build dir).
    const char *path = "test_rknn_image_common_labels.tmp";
    FILE *fp = std::fopen(path, "w");
    CHECK(fp != nullptr, "open labels tmp file for write");
    if (!fp) return;
    std::fprintf(fp, "person\nbicycle\ncar\nmotorcycle\nairplane\nbus\n");
    std::fclose(fp);

    std::vector<std::string> labels;
    std::string err;
    bool ok = loadLabels(path, labels, err);
    CHECK(ok, "loadLabels should succeed");
    CHECK_EQ(labels.size(), static_cast<size_t>(6), "6 labels");
    if (labels.size() >= 6) {
        CHECK(labels[0] == "person", "labels[0]=person");
        CHECK(labels[5] == "bus",    "labels[5]=bus");
    }
    std::remove(path);

    // Non-existent path
    CHECK(!loadLabels("nonexistent_path_labels_tmp.txt", labels, err),
          "missing labels file -> false");
}

// ============================================================
// Bonus: parseMetadata round-trip
// ============================================================
void testParseMetadataRoundTrip()
{
    const char *path = "test_rknn_image_common_meta.tmp";
    FILE *fp = std::fopen(path, "w");
    CHECK(fp != nullptr, "open metadata tmp file for write");
    if (!fp) return;
    std::fprintf(fp,
        "model_sha256=AAAA\n"
        "onnx_sha256=BBBB\n"
        "provenance=yolov5s.onnx via convert_onnx_to_rknn.py\n"
        "labels_file=coco_80_labels.txt\n"
        "decode_variant=yolov5_v6\n"
        "input_width=640\n"
        "input_height=640\n"
        "input_channels=3\n"
        "input_layout=NHWC\n"
        "input_type=FLOAT16\n"
        "caller_input_fmt=NHWC\n"
        "caller_input_type=UINT8\n"
        "caller_input_pass_through=0\n"
        "class_count=80\n"
        "channel_count=255\n"
        "output_count=3\n"
        "output_layout=NCHW\n"
        "output_type=FLOAT16\n"
        "output_request=FLOAT32\n"
        "output_0_dims=1,255,80,80\n"
        "output_1_dims=1,255,40,40\n"
        "output_2_dims=1,255,20,20\n"
        "output_sigmoid=builtin\n"
        "grid_sensitivity=2.0\n"
        "letterbox_fill=114\n"
        "strides=8,16,32\n"
        "anchors_stride8=10,13,16,30,33,23\n"
        "anchors_stride16=30,61,62,45,59,119\n"
        "anchors_stride32=116,90,156,198,373,326\n"
        "golden_image=bus.jpg\n"
        "golden_image_sha256=33b198a1d2839bb9ac4c65d61f9e852196793cae9a0781360859425f6022b69c\n"
        "expected_class=bus\n"
        "expected_class_index=5\n");
    std::fclose(fp);

    Metadata m;
    std::string err;
    bool ok = parseMetadata(path, m, err);
    CHECK(ok, "parseMetadata should succeed");
    if (!ok) { std::remove(path); return; }
    CHECK(m.modelSha256 == "AAAA", "modelSha256=AAAA");
    CHECK_EQ(m.classCount, 80, "classCount=80");
    CHECK_EQ(m.inputWidth, 640, "inputWidth=640");
    CHECK_EQ(m.inputHeight, 640, "inputHeight=640");
    CHECK_EQ(m.callerInputFormat, "NHWC", "callerInputFormat=NHWC");
    CHECK_EQ(m.callerInputType, "UINT8", "callerInputType=UINT8");
    CHECK_EQ(m.inputChannels, 3, "inputChannels=3");
    CHECK_EQ(m.outputCount, 3, "outputCount=3");
    CHECK_EQ(m.channelCount, 255, "channelCount=255");
    CHECK(m.outputDims[0] == std::vector<int>({1,255,80,80}), "output0 dims");
    CHECK(m.outputSigmoidBuiltin, "builtin sigmoid");
    CHECK_EQ(static_cast<int>(m.strides.size()), 3, "3 strides");
    if (m.strides.size() == 3) {
        CHECK_EQ(m.strides[0], 8,  "stride[0]=8");
        CHECK_EQ(m.strides[1], 16, "stride[1]=16");
        CHECK_EQ(m.strides[2], 32, "stride[2]=32");
    }
    CHECK_EQ(static_cast<int>(m.anchorsStride8.size()), 6, "6 anchors stride8");
    CHECK_EQ(m.expectedClass, "bus", "expectedClass=bus");
    CHECK_EQ(m.expectedClassIndex, 5, "expectedClassIndex=5");

    // Validate against matching SHA
    std::string verr;
    CHECK(validateMetadata(m, "AAAA", verr), "validate against matching SHA");
    std::remove(path);
}

// ============================================================
// Bonus: iou basic checks
// ============================================================
void testIou()
{
    Box a{0, 0, 10, 10};
    Box b{0, 0, 10, 10};
    // Same box -> IoU = 1.0
    CHECK(std::fabs(iou(a, b) - 1.0f) < 1e-5f, "identical -> IoU 1.0");

    // No overlap
    Box c{100, 100, 10, 10};
    CHECK(std::fabs(iou(a, c) - 0.0f) < 1e-5f, "no overlap -> IoU 0.0");

    // Zero-area box -> IoU = 0
    Box z{0, 0, 0, 0};
    CHECK(std::fabs(iou(a, z) - 0.0f) < 1e-5f, "zero area -> IoU 0.0");
}

} // namespace

int main()
{
    std::printf("=== test_rknn_image_common (14 TDD items) ===\n");

    testMetadataShaMatchMismatch();         // 1
    testMetadataCallerInputContract();
    testNormalizeBoundaryBoxes();
    testMetadataClassCountValidation();     // 2
    testChannelCount255();                  // 3
    testNchwChannelMajorIndex();            // 4
    testStrideFromGrid();                   // 5
    testLetterboxPadding();                 // 6
    testReverseLetterboxTransform();        // 7
    testReverseLetterboxBoundaryClip();     // 8
    testNanInfRejection();                  // 9
    testSameClassNms();                     // 10
    testDifferentClassNotSuppressed();      // 11
    testMaxDetectionsCap();                 // 12
    testEmptyDetection();                   // 13
    testExpectClassValidation();            // 14
    testClassIndexByName();

    // Bonus coverage (not in the required 14, but exercises file I/O paths)
    testLoadLabelsRoundTrip();
    testParseMetadataRoundTrip();
    testIou();

    std::printf("=== summary: PASS=%d FAIL=%d ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
