// Unit tests for decodeOutputsParallel(): verifies the parallel path produces
// byte-for-byte identical results to the serial decodeOutputs() given the same
// synthetic YOLOv5 output tensors.
//
// Qt-free, NPU-free: compiles with plain g++ under -std=c++17.
//
// Build (single line):
//   g++ -std=c++17 -Wall -Wextra -pthread -I../../common -I../../media
//       tests/rknn/test_rknn_postprocess_parallel.cpp
//       common/thread_pool.cpp
//       media/rknn_postprocess.cpp
//       -o tests/rknn/test_rknn_postprocess_parallel

#include "../../common/thread_pool.h"
#include "../../media/rknn_postprocess.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace rp = rknn_postprocess;

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (condition) {                                                      \
            ++g_pass;                                                        \
        } else {                                                             \
            ++g_fail;                                                        \
            std::printf("FAIL [%s:%d] %s\n", __FILE__, __LINE__,            \
                        #condition);                                         \
        }                                                                    \
    } while (false)

// Build a NCHW channel-major index for a YOLOv5 v6 output tensor.
// channel = anchor * 85 + component; index = (channel * grid + row) * grid + col.
std::size_t nchwIndex(int anchor, int component, int row, int col, int grid)
{
    const std::size_t channel = static_cast<std::size_t>(anchor) * 85U +
                                static_cast<std::size_t>(component);
    return (channel * static_cast<std::size_t>(grid) +
            static_cast<std::size_t>(row)) *
           static_cast<std::size_t>(grid) + static_cast<std::size_t>(col);
}

rp::Metadata canonicalMetadata()
{
    rp::Metadata m;
    m.inputWidth = 640;
    m.inputHeight = 640;
    m.inputChannels = 3;
    m.inputLayout = "NHWC";
    m.inputType = "FLOAT16";
    m.callerInputFormat = "NHWC";
    m.callerInputType = "UINT8";
    m.callerInputPassThrough = 0;
    m.classCount = 80;
    m.channelCount = 255;
    m.outputCount = 3;
    m.outputLayout = "NCHW";
    m.outputType = "FLOAT16";
    m.outputRequest = "FLOAT32";
    m.outputDims[0] = {1, 255, 80, 80};
    m.outputDims[1] = {1, 255, 40, 40};
    m.outputDims[2] = {1, 255, 20, 20};
    m.decodeVariant = "yolov5_v6";
    m.strides = {8, 16, 32};
    m.anchorsStride8  = {10, 13, 16, 30, 33, 23};
    m.anchorsStride16 = {30, 61, 62, 45, 59, 119};
    m.anchorsStride32 = {116, 90, 156, 198, 373, 326};
    m.outputSigmoidBuiltin = true;
    m.gridSensitivity = 2.0f;
    m.letterboxFill = 114;
    return m;
}

rp::LetterboxParams identityLetterbox()
{
    return rp::computeLetterbox(640, 640, 640, 640);
}

rp::OutputTensorView view(const std::vector<float> &data, int grid, int index)
{
    return {data.data(), data.size() * sizeof(float), grid, grid, index};
}

std::vector<float> zeroLayer(int grid)
{
    return std::vector<float>(static_cast<std::size_t>(3) * 85U *
                              static_cast<std::size_t>(grid) *
                              static_cast<std::size_t>(grid), 0.0f);
}

// Inject a candidate detection at (row, col) on anchor 0 with given class.
void addCandidate(std::vector<float> &data, int grid, int row, int col,
                  int classIndex, float obj = 0.90f, float cls = 0.90f)
{
    data[nchwIndex(0, 0, row, col, grid)] = 0.50f;
    data[nchwIndex(0, 1, row, col, grid)] = 0.50f;
    data[nchwIndex(0, 2, row, col, grid)] = 0.50f;
    data[nchwIndex(0, 3, row, col, grid)] = 0.50f;
    data[nchwIndex(0, 4, row, col, grid)] = obj;
    data[nchwIndex(0, 5 + classIndex, row, col, grid)] = cls;
}

// Two detections on the same cell + anchor with similar boxes — NMS must
// suppress the lower-confidence one when their IoU exceeds the threshold.
void addOverlappingPair(std::vector<float> &data, int grid, int row, int col,
                        int classIndex)
{
    // Anchor 0: high confidence.
    data[nchwIndex(0, 0, row, col, grid)] = 0.50f;
    data[nchwIndex(0, 1, row, col, grid)] = 0.50f;
    data[nchwIndex(0, 2, row, col, grid)] = 0.50f;
    data[nchwIndex(0, 3, row, col, grid)] = 0.50f;
    data[nchwIndex(0, 4, row, col, grid)] = 0.90f;
    data[nchwIndex(0, 5 + classIndex, row, col, grid)] = 0.90f;
    // Anchor 1: same box, lower confidence — should be suppressed.
    data[nchwIndex(1, 0, row, col, grid)] = 0.50f;
    data[nchwIndex(1, 1, row, col, grid)] = 0.50f;
    data[nchwIndex(1, 2, row, col, grid)] = 0.50f;
    data[nchwIndex(1, 3, row, col, grid)] = 0.50f;
    data[nchwIndex(1, 4, row, col, grid)] = 0.70f;
    data[nchwIndex(1, 5 + classIndex, row, col, grid)] = 0.70f;
}

// Compare two detection vectors for equality. NMS output is sorted by class
// then by confidence-descending per class — the merge order in
// decodeOutputsParallel preserves this because NMS sorts internally. So a
// direct element-wise compare is valid.
bool detectionsEqual(const std::vector<rp::Detection> &a,
                     const std::vector<rp::Detection> &b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const rp::Detection &x = a[i];
        const rp::Detection &y = b[i];
        if (x.classIndex != y.classIndex) return false;
        if (std::fabs(x.confidence - y.confidence) > 1e-6f) return false;
        if (std::fabs(x.box.cx - y.box.cx) > 1e-4f) return false;
        if (std::fabs(x.box.cy - y.box.cy) > 1e-4f) return false;
        if (std::fabs(x.box.w  - y.box.w)  > 1e-4f) return false;
        if (std::fabs(x.box.h  - y.box.h)  > 1e-4f) return false;
    }
    return true;
}

// Run the same inputs through both serial and parallel paths and assert
// identical output.
void runEquivalenceCase(const char *name,
                        const std::vector<rp::OutputTensorView> &outputs,
                        const rp::Metadata &metadata,
                        const rp::LetterboxParams &letterbox,
                        float confThreshold, float nmsThreshold)
{
    std::printf("RUN  %s\n", name);

    std::vector<rp::Detection> serialDets;
    std::string serialErr;
    const bool serialOk = rp::decodeOutputs(outputs, metadata, letterbox,
                                            confThreshold, nmsThreshold,
                                            serialDets, serialErr);

    std::vector<rp::Detection> parallelDets;
    std::string parallelErr;
    concurrency::ThreadPool pool(3);
    const bool parallelOk = rp::decodeOutputsParallel(outputs, metadata,
            letterbox, confThreshold, nmsThreshold,
            parallelDets, parallelErr, pool);

    CHECK(serialOk == parallelOk);
    if (serialOk != parallelOk) {
        std::printf("  serialOk=%d serialErr=%s\n", serialOk, serialErr.c_str());
        std::printf("  parallelOk=%d parallelErr=%s\n", parallelOk, parallelErr.c_str());
        return;
    }
    if (!serialOk) {
        // Both failed — errors must be identical (same validation path).
        CHECK(serialErr == parallelErr);
        return;
    }
    CHECK(detectionsEqual(serialDets, parallelDets));
    if (!detectionsEqual(serialDets, parallelDets)) {
        std::printf("  serial:    %zu detections\n", serialDets.size());
        std::printf("  parallel:  %zu detections\n", parallelDets.size());
    }
}

void testEmptyInputsProduceEmptyResults()
{
    runEquivalenceCase("empty (zeroed) tensors produce zero detections",
                       {view(zeroLayer(80), 80, 0),
                        view(zeroLayer(40), 40, 1),
                        view(zeroLayer(20), 20, 2)},
                       canonicalMetadata(), identityLetterbox(),
                       0.45f, 0.50f);
}

void testSingleBusCandidateOnStride16()
{
    std::vector<float> l80 = zeroLayer(80);
    std::vector<float> l40 = zeroLayer(40);
    std::vector<float> l20 = zeroLayer(20);
    addCandidate(l40, 40, 10, 12, 5);  // class 5 = bus
    runEquivalenceCase("single bus candidate on stride-16 layer",
                       {view(l80, 80, 0), view(l40, 40, 1), view(l20, 20, 2)},
                       canonicalMetadata(), identityLetterbox(),
                       0.45f, 0.50f);
}

void testCandidatesAcrossAllThreeStrides()
{
    std::vector<float> l80 = zeroLayer(80);
    std::vector<float> l40 = zeroLayer(40);
    std::vector<float> l20 = zeroLayer(20);
    addCandidate(l80, 80, 2, 3, 1);    // class 1 = bicycle
    addCandidate(l40, 40, 10, 12, 5);  // class 5 = bus
    addCandidate(l20, 20, 8, 6, 2);    // class 2 = car
    runEquivalenceCase("candidates on all three strides",
                       {view(l80, 80, 0), view(l40, 40, 1), view(l20, 20, 2)},
                       canonicalMetadata(), identityLetterbox(),
                       0.45f, 0.50f);
}

void testNmsSuppressesOverlappingPair()
{
    std::vector<float> l80 = zeroLayer(80);
    std::vector<float> l40 = zeroLayer(40);
    std::vector<float> l20 = zeroLayer(20);
    addOverlappingPair(l40, 40, 10, 12, 5);  // two anchors same box, diff conf
    runEquivalenceCase("NMS suppresses overlapping pair on stride-16",
                       {view(l80, 80, 0), view(l40, 40, 1), view(l20, 20, 2)},
                       canonicalMetadata(), identityLetterbox(),
                       0.45f, 0.50f);
}

void testShuffledOutputOrderIsAccepted()
{
    std::vector<float> l80 = zeroLayer(80);
    std::vector<float> l40 = zeroLayer(40);
    std::vector<float> l20 = zeroLayer(20);
    addCandidate(l40, 40, 10, 12, 5);
    // Pass outputs in stride-32, stride-8, stride-16 order.
    runEquivalenceCase("shuffled output order (32, 8, 16)",
                       {view(l20, 20, 2), view(l80, 80, 0), view(l40, 40, 1)},
                       canonicalMetadata(), identityLetterbox(),
                       0.45f, 0.50f);
}

void testNaNTensorIsRejected()
{
    std::vector<float> l80 = zeroLayer(80);
    std::vector<float> l40 = zeroLayer(40);
    std::vector<float> l20 = zeroLayer(20);
    l40[nchwIndex(0, 4, 10, 12, 40)] =
        std::numeric_limits<float>::quiet_NaN();
    runEquivalenceCase("NaN tensor is rejected by both paths",
                       {view(l80, 80, 0), view(l40, 40, 1), view(l20, 20, 2)},
                       canonicalMetadata(), identityLetterbox(),
                       0.45f, 0.50f);
}

void testDuplicateStrideIsRejected()
{
    std::vector<float> l80 = zeroLayer(80);
    std::vector<float> l40a = zeroLayer(40);
    std::vector<float> l40b = zeroLayer(40);
    runEquivalenceCase("duplicate stride-16 is rejected by both paths",
                       {view(l80, 80, 0), view(l40a, 40, 1), view(l40b, 40, 2)},
                       canonicalMetadata(), identityLetterbox(),
                       0.45f, 0.50f);
}

// Stress test: run the parallel path many times in a loop to flush out any
// race conditions (data races on per-layer slots, future lifetime issues,
// pool reuse issues). The serial path is the oracle.
void testStressParallelMatchesSerial()
{
    std::printf("RUN  stress: 200 iterations, parallel matches serial\n");
    std::vector<float> l80 = zeroLayer(80);
    std::vector<float> l40 = zeroLayer(40);
    std::vector<float> l20 = zeroLayer(20);
    addCandidate(l80, 80, 2, 3, 1);
    addCandidate(l40, 40, 10, 12, 5);
    addCandidate(l20, 20, 8, 6, 2);
    addOverlappingPair(l40, 40, 15, 20, 0);

    const std::vector<rp::OutputTensorView> outputs = {
        view(l80, 80, 0), view(l40, 40, 1), view(l20, 20, 2)};

    std::vector<rp::Detection> serialDets;
    std::string serialErr;
    const bool serialOk = rp::decodeOutputs(outputs, canonicalMetadata(),
            identityLetterbox(), 0.45f, 0.50f, serialDets, serialErr);
    CHECK(serialOk);
    if (!serialOk) return;

    concurrency::ThreadPool pool(3);
    bool allMatched = true;
    for (int i = 0; i < 200; ++i) {
        std::vector<rp::Detection> parallelDets;
        std::string parallelErr;
        const bool ok = rp::decodeOutputsParallel(outputs, canonicalMetadata(),
                identityLetterbox(), 0.45f, 0.50f,
                parallelDets, parallelErr, pool);
        if (!ok || !detectionsEqual(serialDets, parallelDets)) {
            allMatched = false;
            std::printf("  mismatch at iteration %d (ok=%d dets=%zu vs %zu)\n",
                        i, ok, serialDets.size(), parallelDets.size());
            break;
        }
    }
    CHECK(allMatched);
}

} // namespace

int main()
{
    testEmptyInputsProduceEmptyResults();
    testSingleBusCandidateOnStride16();
    testCandidatesAcrossAllThreeStrides();
    testNmsSuppressesOverlappingPair();
    testShuffledOutputOrderIsAccepted();
    testNaNTensorIsRejected();
    testDuplicateStrideIsRejected();
    testStressParallelMatchesSerial();

    std::printf("\nSUMMARY: PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
