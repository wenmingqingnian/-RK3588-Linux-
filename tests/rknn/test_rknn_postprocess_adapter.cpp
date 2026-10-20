// Host-side contract tests for the production RKNN multi-output adapter.
// The test remains independent of Qt, RKNN headers, and NPU hardware.

#include "../../media/rknn_postprocess.h"

#include <cmath>
#include <cstdio>
#include <limits>
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

std::size_t nchwIndex(int anchor, int component,
                      int row, int col, int grid)
{
    const std::size_t channel = static_cast<std::size_t>(anchor) * 85U +
                                static_cast<std::size_t>(component);
    return (channel * static_cast<std::size_t>(grid) +
            static_cast<std::size_t>(row)) *
           static_cast<std::size_t>(grid) + static_cast<std::size_t>(col);
}

rp::Metadata metadata()
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
    m.anchorsStride8 = {10, 13, 16, 30, 33, 23};
    m.anchorsStride16 = {30, 61, 62, 45, 59, 119};
    m.anchorsStride32 = {116, 90, 156, 198, 373, 326};
    m.outputSigmoidBuiltin = true;
    m.gridSensitivity = 2.0f;
    m.letterboxFill = 114;
    return m;
}

void testFloatTensorQuantizationContract()
{
    std::printf("RUN  float tensor quantization contract\n");
    std::string error;

    CHECK(rp::validateFloatTensorQuantization(
        rp::FloatTensorQuantization::None, 0, 1.0f, error));
    CHECK(error.empty());

    CHECK(rp::validateFloatTensorQuantization(
        rp::FloatTensorQuantization::Affine, 0, 1.0f, error));
    CHECK(error.empty());
    CHECK(rp::validateFloatTensorQuantization(
        rp::FloatTensorQuantization::Affine, 0,
        1.0f + std::numeric_limits<float>::epsilon(), error));

    CHECK(!rp::validateFloatTensorQuantization(
        rp::FloatTensorQuantization::Affine, 1, 1.0f, error));
    CHECK(error.find("zero point") != std::string::npos);
    CHECK(!rp::validateFloatTensorQuantization(
        rp::FloatTensorQuantization::Affine, 0, 0.5f, error));
    CHECK(error.find("unit scale") != std::string::npos);
    CHECK(!rp::validateFloatTensorQuantization(
        rp::FloatTensorQuantization::Affine, 0,
        std::numeric_limits<float>::quiet_NaN(), error));
    CHECK(error.find("finite") != std::string::npos);
    CHECK(!rp::validateFloatTensorQuantization(
        rp::FloatTensorQuantization::Affine, 0,
        std::numeric_limits<float>::infinity(), error));
    CHECK(error.find("finite") != std::string::npos);
    CHECK(!rp::validateFloatTensorQuantization(
        rp::FloatTensorQuantization::Dfp, 0, 1.0f, error));
    CHECK(error.find("DFP") != std::string::npos);
    CHECK(!rp::validateFloatTensorQuantization(
        rp::FloatTensorQuantization::Unsupported, 0, 1.0f, error));
    CHECK(error.find("unsupported") != std::string::npos);
}

void testCanonicalRuntimeMetadataContract()
{
    std::printf("RUN  canonical runtime metadata contract\n");
    const std::string path =
        std::string(RKNN_TEST_PROJECT_ROOT) + "/config/yolov5s_metadata.conf";
    rp::Metadata parsed;
    std::string error;
    const bool ok = rp::parseMetadata(path, parsed, error);

    CHECK(ok);
    if (!ok) {
        std::printf("metadata parse error: %s\n", error.c_str());
        return;
    }

    CHECK(error.empty());
    CHECK(parsed.outputLayout == "NCHW");
    CHECK(parsed.outputType == "FLOAT16");
    CHECK(parsed.outputRequest == "FLOAT32");
    CHECK(parsed.outputSigmoidBuiltin);
    CHECK(parsed.strides == std::vector<int>({8, 16, 32}));
    CHECK(parsed.modelSha256 ==
          "85287823c5909c4f47396c4fdbc53db0abc2ed411c981f8fa872df649572dac9");
    CHECK(parsed.inputWidth == 640);
    CHECK(parsed.inputHeight == 640);
    CHECK(parsed.callerInputFormat == "NHWC");
    CHECK(parsed.callerInputType == "UINT8");
    CHECK(parsed.inputChannels == 3);
    CHECK(parsed.inputLayout == "NHWC");
    CHECK(parsed.inputType == "FLOAT16");
    CHECK(parsed.callerInputPassThrough == 0);
    CHECK(parsed.outputCount == 3);
    CHECK(parsed.channelCount == 255);
    CHECK(parsed.outputDims[2] == std::vector<int>({1,255,20,20}));
    CHECK(parsed.gridSensitivity == 2.0f);
    CHECK(parsed.letterboxFill == 114);
    CHECK(parsed.classCount == 80);
    CHECK(parsed.decodeVariant == "yolov5_v6");
}

rp::OutputTensorView view(const std::vector<float> &data,
                          int grid, int index)
{
    return {data.data(), data.size() * sizeof(float), grid, grid, index};
}

std::vector<float> layer(int grid)
{
    return std::vector<float>(static_cast<std::size_t>(3) * 85U *
                              static_cast<std::size_t>(grid) *
                              static_cast<std::size_t>(grid),
                              0.0f);
}

void addBusCandidate(std::vector<float> &layer40)
{
    layer40[nchwIndex(0, 0, 10, 12, 40)] = 0.50f;
    layer40[nchwIndex(0, 1, 10, 12, 40)] = 0.50f;
    layer40[nchwIndex(0, 2, 10, 12, 40)] = 0.50f;
    layer40[nchwIndex(0, 3, 10, 12, 40)] = 0.50f;
    layer40[nchwIndex(0, 4, 10, 12, 40)] = 0.90f;
    layer40[nchwIndex(0, 5 + 5, 10, 12, 40)] = 0.90f;
}

void addCandidate(std::vector<float> &data, int grid, int row, int col,
                  int classIndex)
{
    data[nchwIndex(0, 0, row, col, grid)] = 0.50f;
    data[nchwIndex(0, 1, row, col, grid)] = 0.50f;
    data[nchwIndex(0, 2, row, col, grid)] = 0.50f;
    data[nchwIndex(0, 3, row, col, grid)] = 0.50f;
    data[nchwIndex(0, 4, row, col, grid)] = 0.90f;
    data[nchwIndex(0, 5 + classIndex, row, col, grid)] = 0.90f;
}

rp::LetterboxParams identityLetterbox()
{
    return rp::computeLetterbox(640, 640, 640, 640);
}

void testShuffledOutputsDecodeOneBuiltinSigmoidBus()
{
    std::printf("RUN  shuffled outputs decode one builtin-sigmoid bus\n");
    std::vector<float> layer80 = layer(80);
    std::vector<float> layer40 = layer(40);
    std::vector<float> layer20 = layer(20);
    addBusCandidate(layer40);

    std::vector<rp::Detection> detections;
    std::string error;
    const bool ok = rp::decodeOutputs(
        {view(layer20, 20, 2), view(layer80, 80, 0), view(layer40, 40, 1)},
        metadata(), identityLetterbox(), 0.45f, 0.50f, detections, error);

    CHECK(ok);
    CHECK(error.empty());
    CHECK(detections.size() == 1);
    if (detections.size() == 1) {
        CHECK(detections[0].classIndex == 5);
        CHECK(std::fabs(detections[0].confidence - 0.81f) < 0.001f);
    }
}

void testDuplicateGridLayerIsRejected()
{
    std::printf("RUN  duplicate 40x40 output layer is rejected\n");
    std::vector<float> layer20 = layer(20);
    std::vector<float> layer40 = layer(40);

    std::vector<rp::Detection> detections;
    std::string error;
    const bool ok = rp::decodeOutputs(
        {view(layer20, 20, 2), view(layer40, 40, 0), view(layer40, 40, 1)},
        metadata(), identityLetterbox(), 0.45f, 0.50f, detections, error);

    CHECK(!ok);
    CHECK(!error.empty());
    CHECK(error.find("duplicate") != std::string::npos);
}

void testCandidatesFromDifferentLayersAreAccumulated()
{
    std::printf("RUN  candidates from different output layers accumulate\n");
    std::vector<float> layer80 = layer(80);
    std::vector<float> layer40 = layer(40);
    std::vector<float> layer20 = layer(20);
    addCandidate(layer80, 80, 2, 3, 1);
    addCandidate(layer20, 20, 12, 14, 2);

    std::vector<rp::Detection> detections;
    std::string error;
    const bool ok = rp::decodeOutputs(
        {view(layer80, 80, 0), view(layer40, 40, 1), view(layer20, 20, 2)},
        metadata(), identityLetterbox(), 0.45f, 0.50f, detections, error);

    CHECK(ok);
    CHECK(error.empty());
    CHECK(detections.size() == 2);
    bool foundClass1 = false;
    bool foundClass2 = false;
    for (const rp::Detection &detection : detections) {
        foundClass1 = foundClass1 || detection.classIndex == 1;
        foundClass2 = foundClass2 || detection.classIndex == 2;
    }
    CHECK(foundClass1);
    CHECK(foundClass2);
}

void testInvalidMetadataSemanticsAreRejected()
{
    std::printf("RUN  invalid decode metadata semantics are rejected\n");
    std::vector<float> layer80 = layer(80);
    std::vector<float> layer40 = layer(40);
    std::vector<float> layer20 = layer(20);
    const std::vector<rp::OutputTensorView> outputs = {
        view(layer80, 80, 0), view(layer40, 40, 1), view(layer20, 20, 2)};

    const auto rejected = [&](rp::Metadata invalid, const char *field) {
        std::vector<rp::Detection> detections;
        std::string error;
        const bool ok = rp::decodeOutputs(outputs, invalid,
            identityLetterbox(), 0.45f, 0.50f, detections, error);
        CHECK(!ok);
        CHECK(error.find(field) != std::string::npos);
    };

    rp::Metadata invalid = metadata();
    invalid.inputWidth = 0;
    rejected(invalid, "input_width");
    invalid = metadata();
    invalid.decodeVariant = "yolov3";
    rejected(invalid, "decode_variant");
    invalid = metadata();
    invalid.outputLayout = "NHWC";
    rejected(invalid, "output_layout");
    invalid = metadata();
    invalid.outputRequest = "FLOAT16";
    rejected(invalid, "output_request");
    invalid = metadata();
    invalid.callerInputFormat = "NCHW";
    rejected(invalid, "caller_input_fmt");
    invalid = metadata();
    invalid.callerInputType = "FLOAT16";
    rejected(invalid, "caller_input_type");
    invalid = metadata();
    invalid.outputSigmoidBuiltin = false;
    rejected(invalid, "output_sigmoid");
    invalid = metadata();
    invalid.callerInputPassThrough = 1;
    rejected(invalid, "caller_input_pass_through");
}

void testInvalidLetterboxContractsAreRejected()
{
    std::printf("RUN  invalid letterbox contracts are rejected\n");
    std::vector<float> layer80 = layer(80);
    std::vector<float> layer40 = layer(40);
    std::vector<float> layer20 = layer(20);
    const std::vector<rp::OutputTensorView> outputs = {
        view(layer80, 80, 0), view(layer40, 40, 1), view(layer20, 20, 2)};

    const auto rejected = [&](const rp::LetterboxParams &invalid) {
        std::vector<rp::Detection> detections(1);
        std::string error;
        const bool ok = rp::decodeOutputs(outputs, metadata(), invalid,
            0.45f, 0.50f, detections, error);
        CHECK(!ok);
        CHECK(error.find("letterbox") != std::string::npos);
        CHECK(detections.empty());
    };

    rejected(rp::LetterboxParams{});

    rp::LetterboxParams invalid = identityLetterbox();
    invalid.scale = std::numeric_limits<float>::quiet_NaN();
    rejected(invalid);
    invalid = identityLetterbox();
    invalid.scale = 0.0f;
    rejected(invalid);
    invalid = identityLetterbox();
    invalid.dstW = 320;
    rejected(invalid);
    invalid = identityLetterbox();
    invalid.srcW = 0;
    rejected(invalid);
    invalid = identityLetterbox();
    invalid.padLeft = -1;
    rejected(invalid);
    invalid = identityLetterbox();
    invalid.padRight = 1;
    rejected(invalid);
}

void testUndersizedBufferIsRejected()
{
    std::printf("RUN  undersized output byte count is rejected\n");
    std::vector<float> layer80 = layer(80);
    std::vector<float> layer40 = layer(40);
    std::vector<float> layer20 = layer(20);
    rp::OutputTensorView undersized = view(layer40, 40, 1);
    undersized.byteSize -= sizeof(float);

    std::vector<rp::Detection> detections;
    std::string error;
    const bool ok = rp::decodeOutputs(
        {view(layer80, 80, 0), undersized, view(layer20, 20, 2)},
        metadata(), identityLetterbox(), 0.45f, 0.50f, detections, error);

    CHECK(!ok);
    CHECK(!error.empty());
}

void testMissingTwentyGridLayerIsRejected()
{
    std::printf("RUN  missing 20x20 output layer is rejected\n");
    std::vector<float> layer80 = layer(80);
    std::vector<float> layer40 = layer(40);

    std::vector<rp::Detection> detections;
    std::string error;
    const bool ok = rp::decodeOutputs(
        {view(layer80, 80, 0), view(layer40, 40, 1)},
        metadata(), identityLetterbox(), 0.45f, 0.50f, detections, error);

    CHECK(!ok);
    CHECK(!error.empty());
}

void testNanInputIsRejected()
{
    std::printf("RUN  NaN tensor input is rejected\n");
    std::vector<float> layer80 = layer(80);
    std::vector<float> layer40 = layer(40);
    std::vector<float> layer20 = layer(20);
    layer40[nchwIndex(0, 4, 10, 12, 40)] =
        std::numeric_limits<float>::quiet_NaN();

    std::vector<rp::Detection> detections;
    std::string error;
    const bool ok = rp::decodeOutputs(
        {view(layer80, 80, 0), view(layer40, 40, 1), view(layer20, 20, 2)},
        metadata(), identityLetterbox(), 0.45f, 0.50f, detections, error);

    CHECK(!ok);
    CHECK(!error.empty());
}

} // namespace

int main()
{
    testFloatTensorQuantizationContract();
    testCanonicalRuntimeMetadataContract();
    testShuffledOutputsDecodeOneBuiltinSigmoidBus();
    testCandidatesFromDifferentLayersAreAccumulated();
    testInvalidMetadataSemanticsAreRejected();
    testInvalidLetterboxContractsAreRejected();
    testDuplicateGridLayerIsRejected();
    testUndersizedBufferIsRejected();
    testMissingTwentyGridLayerIsRejected();
    testNanInputIsRejected();

    std::printf("\nSUMMARY: PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
