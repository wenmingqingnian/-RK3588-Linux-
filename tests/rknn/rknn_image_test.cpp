// rknn_image_test — Stage B static-image RKNN inference tool (jilu.txt §4-§9).
//
// Loads a YOLOv5s RKNN model, runs inference on a golden test image, decodes
// the NCHW outputs using the v6 decode formula, applies class-aware NMS, and
// saves an annotated image with bounding boxes.
//
// Pipeline:
//   parse CLI → compute model SHA256 → parse/validate metadata
//   → load COCO labels → load QImage → letterbox to 640x640
//   → RKNN Runtime: dlopen → initModel → queryInputAttr → queryOutputAttr
//   → inputs_set(UINT8 NHWC, pass_through=0) → run → outputs_get(want_float=1)
//   → build OutputTensorView list → shared decodeOutputs
//   → findExpectedClass
//   → QPainter annotate → save → print key=value log
//
// Build (board-side, jilu.txt §10):
//   cmake -S tests/rknn -B build_rknn_image
//         -DCMAKE_BUILD_TYPE=Release
//         -DRKNN_INCLUDE_DIR=/usr/local/include
//   cmake --build build_rknn_image --target rknn_image_test
//
// Usage:
//   ./rknn_image_test --library /usr/local/lib/librknnrt.so
//       --model ./config/yolov5s.rknn
//       --metadata ./config/yolov5s_metadata.conf
//       --labels  ./tests/rknn/data/coco_80_labels.txt
//       --image   ./tests/rknn/data/bus.jpg
//       --output  /tmp/bus_annotated.jpg
//       --conf    0.25
//       --nms     0.45
//       --expect-class bus

#include "../../media/rknn_runtime.h"
#include "../../media/rknn_postprocess.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPen>
#include <QRectF>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace rknn_image = rknn_postprocess;

namespace {

// ---------- Exit codes (jilu.txt §CLI: 非法参数必须非零退出且不崩溃) ----------
enum ExitCode : int {
    kExitOk                = 0,
    kExitArgError          = 2,
    kExitLibLoadFail       = 3,
    kExitModelReadFail     = 4,
    kExitInitFail          = 5,
    kExitQueryFail         = 6,
    kExitInputsSetFail     = 7,
    kExitRunFail           = 8,
    kExitOutputsFail       = 9,
    kExitInternalFail      = 10,
    kExitMetadataFail      = 11,
    kExitImageFail         = 12,
    kExitLayoutFail        = 13,
};

struct CliArgs {
    std::string libraryPath  = "/usr/local/lib/librknnrt.so";
    std::string modelPath    = "./config/yolov5s.rknn";
    std::string metadataPath = "./config/yolov5s_metadata.conf";
    std::string labelsPath   = "./tests/rknn/data/coco_80_labels.txt";
    std::string imagePath    = "./tests/rknn/data/bus.jpg";
    std::string outputPath   = "/tmp/bus_annotated.jpg";
    float       confThreshold = 0.25f;
    float       nmsThreshold  = 0.45f;
    std::string expectClass;
    bool        expectClassExplicit = false;
    bool        help          = false;
};

static void printHelp(const char *argv0)
{
    std::printf(
        "Usage: %s --library <lib> --model <model.rknn> --metadata <meta.conf>\n"
        "          --labels <labels.txt> --image <img.jpg> --output <out.jpg>\n"
        "          [--conf 0.25] [--nms 0.45] [--expect-class bus] [--help]\n"
        "\n"
        "Options:\n"
        "  --library <path>     Path to librknnrt.so\n"
        "  --model <path>       Path to .rknn model\n"
        "  --metadata <path>    Path to yolov5s_metadata.conf\n"
        "  --labels <path>      Path to coco_80_labels.txt\n"
        "  --image <path>       Path to input image (jpg/png)\n"
        "  --output <path>      Path to write annotated image\n"
        "  --conf <float>       Confidence threshold (default 0.25)\n"
        "  --nms <float>        NMS IoU threshold (default 0.45)\n"
        "  --expect-class <s>   Expected class name (default: metadata)\n"
        "  --help               Show this help\n"
        "\n"
        "Exit codes:\n"
        "  0=ok 2=arg 3=lib 4=model_read 5=init 6=query\n"
        "  7=inputs_set 8=run 9=outputs 10=internal\n"
        "  11=metadata 12=image 13=layout\n",
        argv0);
}

static bool parseArgs(int argc, char **argv, CliArgs &out, std::string &err)
{
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char *flag) -> const char * {
            if (i + 1 >= argc) {
                err = std::string("missing value for ") + flag;
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--help" || a == "-h") {
            out.help = true;
        } else if (a == "--library") {
            const char *v = next("--library");
            if (!v) return false;
            out.libraryPath = v;
        } else if (a == "--model") {
            const char *v = next("--model");
            if (!v) return false;
            out.modelPath = v;
        } else if (a == "--metadata") {
            const char *v = next("--metadata");
            if (!v) return false;
            out.metadataPath = v;
        } else if (a == "--labels") {
            const char *v = next("--labels");
            if (!v) return false;
            out.labelsPath = v;
        } else if (a == "--image") {
            const char *v = next("--image");
            if (!v) return false;
            out.imagePath = v;
        } else if (a == "--output") {
            const char *v = next("--output");
            if (!v) return false;
            out.outputPath = v;
        } else if (a == "--conf") {
            const char *v = next("--conf");
            if (!v) return false;
            try { out.confThreshold = std::stof(std::string(v)); }
            catch (...) { err = "invalid --conf value"; return false; }
        } else if (a == "--nms") {
            const char *v = next("--nms");
            if (!v) return false;
            try { out.nmsThreshold = std::stof(std::string(v)); }
            catch (...) { err = "invalid --nms value"; return false; }
        } else if (a == "--expect-class") {
            const char *v = next("--expect-class");
            if (!v) return false;
            out.expectClass = v;
            out.expectClassExplicit = true;
        } else {
            err = std::string("unknown argument: ") + a;
            return false;
        }
    }

    // Validate thresholds (jilu.txt §CLI: 非法参数必须非零退出).
    if (out.confThreshold < 0.0f || out.confThreshold > 1.0f) {
        err = "--conf must be in [0, 1]";
        return false;
    }
    if (out.nmsThreshold < 0.0f || out.nmsThreshold > 1.0f) {
        err = "--nms must be in [0, 1]";
        return false;
    }
    return true;
}

// Compute SHA256 of a file using Qt's QCryptographicHash.
static std::string computeFileSha256(const std::string &path)
{
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return std::string();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f)) return std::string();
    return hash.result().toHex().toStdString();
}

// ---------- tensor attribute helpers ----------
static const char *tensorTypeStr(rknn_tensor_type t)
{
    switch (t) {
    case RKNN_TENSOR_FLOAT32: return "FLOAT32";
    case RKNN_TENSOR_FLOAT16: return "FLOAT16";
    case RKNN_TENSOR_INT8:    return "INT8";
    case RKNN_TENSOR_UINT8:   return "UINT8";
    default:                  return "OTHER";
    }
}

static const char *tensorFmtStr(rknn_tensor_format f)
{
    switch (f) {
    case RKNN_TENSOR_NCHW:     return "NCHW";
    case RKNN_TENSOR_NHWC:     return "NHWC";
    case RKNN_TENSOR_NC1HWC2:  return "NC1HWC2";
    default:                   return "OTHER";
    }
}

// Print a compact one-line summary of a tensor attribute.
static void printTensorAttrLine(const char *prefix, uint32_t idx,
                                const rknn_tensor_attr &a)
{
    std::printf("%s_%u: fmt=%s type=%s n_dims=%u dims=",
                prefix, idx, tensorFmtStr(a.fmt), tensorTypeStr(a.type),
                a.n_dims);
    for (uint32_t i = 0; i < a.n_dims && i < RKNN_MAX_DIMS; ++i) {
        std::printf("%s%u", (i == 0 ? "" : ","), a.dims[i]);
    }
    std::printf(" n_elems=%u size=%u pass_through=%u\n",
                a.n_elems, a.size, a.pass_through);
}

// Validate an output tensor attribute against YOLOv5s expectations
// (jilu.txt §输出布局验证).
//   - n_dims == 4
//   - fmt == NCHW
//   - dims[0] == 1 (batch)
//   - dims[1] == 255 (channel)
//   - dims[2] == dims[3] (square grid)
//   - stride is one of {8, 16, 32} derived from grid size
// Returns the stride on success, 0 on failure.
static int validateOutputAttr(const rknn_tensor_attr &a, std::string &err)
{
    if (a.n_dims != 4) {
        err = "output n_dims != 4 (got " + std::to_string(a.n_dims) + ")";
        return 0;
    }
    if (a.fmt != RKNN_TENSOR_NCHW) {
        err = std::string("output fmt != NCHW (got ") + tensorFmtStr(a.fmt) + ")";
        return 0;
    }
    if (a.dims[0] != 1) {
        err = "output batch != 1 (got " + std::to_string(a.dims[0]) + ")";
        return 0;
    }
    if (a.dims[1] != rknn_image::kChannelCount) {
        err = "output channel != 255 (got " + std::to_string(a.dims[1]) + ")";
        return 0;
    }
    int gridH = static_cast<int>(a.dims[2]);
    int gridW = static_cast<int>(a.dims[3]);
    if (gridW != gridH) {
        err = "output grid non-square: " + std::to_string(gridW) + "x" +
              std::to_string(gridH);
        return 0;
    }
    int stride = rknn_image::strideFromGrid(gridW, gridH);
    if (stride == 0) {
        err = "unrecognized grid size: " + std::to_string(gridW) + "x" +
              std::to_string(gridH);
        return 0;
    }
    return stride;
}

class OutputsLease {
public:
    OutputsLease(rknn_runtime::Runtime &runtime,
                 std::vector<rknn_output> &outputs) noexcept
        : runtime_(runtime), outputs_(outputs.data()),
          count_(static_cast<uint32_t>(outputs.size())), armed_(true)
    {
    }

    ~OutputsLease() noexcept
    {
        if (armed_) (void)release();
    }

    OutputsLease(const OutputsLease &) = delete;
    OutputsLease &operator=(const OutputsLease &) = delete;

    int release() noexcept
    {
        if (!armed_) return RKNN_SUCC;
        armed_ = false;
        int rc = -1;
        try {
            rc = runtime_.outputsRelease(count_, outputs_);
        } catch (...) {
            rc = -1;
        }
        std::printf("OUTPUTS_RELEASE_RC=%d\n", rc);
        return rc;
    }

private:
    rknn_runtime::Runtime &runtime_;
    rknn_output *outputs_;
    uint32_t count_;
    bool armed_;
};

// Draw a single detection box with class label + confidence.
static void drawDetection(QPainter &p, const rknn_image::Detection &d,
                          int imageW, int imageH)
{
    float x1 = d.box.cx - d.box.w * 0.5f;
    float y1 = d.box.cy - d.box.h * 0.5f;
    float w  = d.box.w;
    float h  = d.box.h;
    if (x1 < 0) { w += x1; x1 = 0; }
    if (y1 < 0) { h += y1; y1 = 0; }
    if (x1 + w > imageW) w = imageW - x1;
    if (y1 + h > imageH) h = imageH - y1;
    if (w <= 0 || h <= 0) return;

    QPen pen(QColor(0, 255, 0), 2);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(x1, y1, w, h));

    // Label text with background.
    QString label = QString::fromStdString(d.className) +
                    QString(" %1").arg(d.confidence, 0, 'f', 2);
    QFont font("Monospace", 10);
    p.setFont(font);
    QFontMetrics fm(font);
    int textW = fm.horizontalAdvance(label);
    int textH = fm.height();
    p.fillRect(QRectF(x1, std::max(0.0f, y1 - textH), textW + 4, textH),
               QColor(0, 0, 0, 180));
    p.setPen(QColor(255, 255, 255));
    p.drawText(QPointF(x1 + 2, std::max(0.0f, y1 - 2)), label);
}

} // namespace

int main(int argc, char **argv)
{
    // QGuiApplication required for QPainter::drawText() font handling.
    // Created before any QImage/QPainter operations.
    QGuiApplication app(argc, argv);

    CliArgs args;
    std::string argErr;
    if (!parseArgs(argc, argv, args, argErr)) {
        std::fprintf(stderr, "ERROR: %s\n", argErr.c_str());
        printHelp(argv[0]);
        return kExitArgError;
    }
    if (args.help) {
        printHelp(argv[0]);
        return kExitOk;
    }

    // ============================================================
    // Step 1: compute model SHA256 (jilu.txt §3, §输出 MODEL_SHA256)
    // ============================================================
    std::string modelSha = computeFileSha256(args.modelPath);
    if (modelSha.empty()) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=cannot compute model SHA256: %s\n",
                    args.modelPath.c_str());
        return kExitModelReadFail;
    }
    std::printf("MODEL_SHA256=%s\n", modelSha.c_str());

    // ============================================================
    // Step 2: parse + validate metadata (jilu.txt §3, §5)
    // ============================================================
    rknn_image::Metadata meta;
    std::string metaErr;
    if (!rknn_image::parseMetadata(args.metadataPath, meta, metaErr)) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=parseMetadata: %s\n", metaErr.c_str());
        return kExitMetadataFail;
    }
    if (!rknn_image::validateMetadata(meta, modelSha, metaErr)) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=validateMetadata: %s\n", metaErr.c_str());
        return kExitMetadataFail;
    }
    std::printf("METADATA_PATH=%s\n", args.metadataPath.c_str());
    std::printf("DECODE_VARIANT=%s\n", meta.decodeVariant.c_str());
    std::printf("OUTPUT_SIGMOID=%s\n", meta.outputSigmoidBuiltin ? "builtin" : "logits");

    // ============================================================
    // Step 3: load COCO labels (jilu.txt §3)
    // ============================================================
    std::vector<std::string> labels;
    std::string labelsErr;
    if (!rknn_image::loadLabels(args.labelsPath, labels, labelsErr)) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=loadLabels: %s\n", labelsErr.c_str());
        return kExitMetadataFail;
    }
    if (static_cast<int>(labels.size()) != rknn_image::kCocoClassCount) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=labels count != %d (got %zu)\n",
                    rknn_image::kCocoClassCount, labels.size());
        return kExitMetadataFail;
    }

    // An explicit CLI name must be nonempty and resolve exactly. Only an
    // omitted --expect-class flag falls back to the metadata expectation.
    std::string expectClassName;
    int expectClassIndex = -1;
    if (args.expectClassExplicit) {
        if (args.expectClass.empty()) {
            std::printf("RKNN_IMAGE_RESULT=FAIL\n");
            std::printf("ERROR=--expect-class must not be empty\n");
            return kExitArgError;
        }
        expectClassName = args.expectClass;
        expectClassIndex =
            rknn_image::findClassIndexByName(labels, expectClassName);
        if (expectClassIndex < 0) {
            std::printf("RKNN_IMAGE_RESULT=FAIL\n");
            std::printf("ERROR=unknown --expect-class: %s\n",
                        expectClassName.c_str());
            return kExitArgError;
        }
    } else {
        expectClassName = meta.expectedClass;
        expectClassIndex =
            rknn_image::findClassIndexByName(labels, expectClassName);
        if (expectClassIndex < 0 && meta.expectedClassIndex >= 0 &&
            meta.expectedClassIndex < static_cast<int>(labels.size())) {
            expectClassIndex = meta.expectedClassIndex;
            expectClassName = labels[expectClassIndex];
        }
        if (expectClassIndex < 0) {
            std::printf("RKNN_IMAGE_RESULT=FAIL\n");
            std::printf("ERROR=metadata expected class is not in labels\n");
            return kExitMetadataFail;
        }
    }
    std::printf("EXPECTED_CLASS=%s\n", expectClassName.c_str());
    std::printf("EXPECTED_CLASS_INDEX=%d\n", expectClassIndex);

    // ============================================================
    // Step 4: load image with QImage (jilu.txt §6)
    // ============================================================
    auto tPre0 = std::chrono::steady_clock::now();

    QImage img(QString::fromStdString(args.imagePath));
    if (img.isNull()) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=QImage load failed: %s\n", args.imagePath.c_str());
        return kExitImageFail;
    }
    QImage rgb = img.convertToFormat(QImage::Format_RGB888);
    if (rgb.isNull()) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=convertToFormat RGB888 failed\n");
        return kExitImageFail;
    }

    int srcW = rgb.width();
    int srcH = rgb.height();
    int srcBpl = rgb.bytesPerLine();
    std::printf("SOURCE_WIDTH=%d\n",  srcW);
    std::printf("SOURCE_HEIGHT=%d\n", srcH);
    std::printf("SOURCE_BYTES_PER_LINE=%d\n", srcBpl);

    // Compute image SHA256 for traceability (jilu.txt §输出 IMAGE_SHA256).
    std::string imageSha = computeFileSha256(args.imagePath);
    std::printf("IMAGE_SHA256=%s\n", imageSha.c_str());

    // ============================================================
    // Step 5: letterbox preprocess (jilu.txt §6)
    // ============================================================
    int modelW = meta.inputWidth;
    int modelH = meta.inputHeight;
    std::printf("MODEL_WIDTH=%d\n",  modelW);
    std::printf("MODEL_HEIGHT=%d\n", modelH);

    rknn_image::LetterboxParams lb =
        rknn_image::computeLetterbox(srcW, srcH, modelW, modelH);
    std::printf("LETTERBOX_SCALE=%.6f\n", lb.scale);
    std::printf("PAD_LEFT=%d\n",   lb.padLeft);
    std::printf("PAD_TOP=%d\n",    lb.padTop);
    std::printf("PAD_RIGHT=%d\n",  lb.padRight);
    std::printf("PAD_BOTTOM=%d\n", lb.padBottom);

    // Build the UINT8 NHWC input buffer.
    // QImage may have a stride > width*3 (bytesPerLine padding); we must
    // repack to a tight W*H*3 buffer for RKNN (jilu.txt §6: 正确处理 bytesPerLine).
    std::vector<uint8_t> inputBuf(static_cast<size_t>(modelW) * modelH * 3,
                                  static_cast<uint8_t>(rknn_image::kLetterboxFill));

    // First extract a tight srcW*srcH*3 RGB buffer from QImage.
    std::vector<uint8_t> srcTight(static_cast<size_t>(srcW) * srcH * 3);
    for (int y = 0; y < srcH; ++y) {
        const uint8_t *row = rgb.constScanLine(y);
        std::memcpy(srcTight.data() + static_cast<size_t>(y) * srcW * 3,
                    row, static_cast<size_t>(srcW) * 3);
    }

    // Apply letterbox: scale + pad into inputBuf.
    if (!rknn_image::applyLetterbox(srcTight.data(), srcW, srcH,
                                    inputBuf.data(), modelW, modelH, lb,
                                    static_cast<uint8_t>(rknn_image::kLetterboxFill))) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=applyLetterbox failed\n");
        return kExitInternalFail;
    }

    auto tPre1 = std::chrono::steady_clock::now();
    long preUs = std::chrono::duration_cast<std::chrono::microseconds>(
                     tPre1 - tPre0).count();
    std::printf("PREPROCESS_TIME_US=%ld\n", preUs);

    // ============================================================
    // Step 6: load RKNN runtime + init model (jilu.txt §7)
    // ============================================================
    rknn_runtime::Runtime rt;
    if (!rt.loadLibrary(args.libraryPath)) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=loadLibrary: %s\n", rt.lastError().c_str());
        return kExitLibLoadFail;
    }
    if (!rt.initModel(args.modelPath)) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=initModel: %s\n", rt.lastError().c_str());
        const std::string &e = rt.lastError();
        if (e.find("rknn_init failed") == 0) return kExitInitFail;
        return kExitModelReadFail;
    }

    // Query SDK version + IO count.
    rknn_sdk_version ver{};
    if (!rt.querySdkVersion(ver)) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=querySdkVersion: %s\n", rt.lastError().c_str());
        return kExitQueryFail;
    }
    std::printf("RKNN_API_VERSION=%s\n", ver.api_version);
    std::printf("RKNN_DRIVER_VERSION=%s\n", ver.drv_version);

    rknn_input_output_num ioNum{};
    if (!rt.queryIoNum(ioNum)) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=queryIoNum: %s\n", rt.lastError().c_str());
        return kExitQueryFail;
    }
    std::printf("INPUT_COUNT=%u\n",  ioNum.n_input);
    std::printf("OUTPUT_COUNT=%u\n", ioNum.n_output);

    if (ioNum.n_input != 1) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=expected 1 input, got %u\n", ioNum.n_input);
        return kExitLayoutFail;
    }
    if (ioNum.n_output != 3) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=expected 3 outputs, got %u\n", ioNum.n_output);
        return kExitLayoutFail;
    }

    // ============================================================
    // Step 7: query + print input/output attrs (jilu.txt §输出布局验证)
    // ============================================================
    rknn_tensor_attr inAttr{};
    if (!rt.queryInputAttr(0, inAttr)) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=queryInputAttr: %s\n", rt.lastError().c_str());
        return kExitQueryFail;
    }
    std::printf("INPUT_ATTRS: ");
    printTensorAttrLine("IN", 0, inAttr);

    std::vector<rknn_tensor_attr> outAttrs(ioNum.n_output);
    for (uint32_t i = 0; i < ioNum.n_output; ++i) {
        if (!rt.queryOutputAttr(i, outAttrs[i])) {
            std::printf("RKNN_IMAGE_RESULT=FAIL\n");
            std::printf("ERROR=queryOutputAttr[%u]: %s\n", i,
                        rt.lastError().c_str());
            return kExitQueryFail;
        }
        std::printf("OUTPUT_ATTRS: ");
        printTensorAttrLine("OUT", i, outAttrs[i]);
    }

    // Validate each output: NCHW, batch=1, channel=255, recognized grid.
    // We do NOT assume outputs[0/1/2] correspond to stride 8/16/32 in any
    // fixed order (jilu.txt §输出布局验证).
    for (uint32_t i = 0; i < ioNum.n_output; ++i) {
        std::string vErr;
        int stride = validateOutputAttr(outAttrs[i], vErr);
        if (stride == 0) {
            std::printf("RKNN_IMAGE_RESULT=FAIL\n");
            std::printf("ERROR=output[%u] layout: %s\n", i, vErr.c_str());
            std::printf("RKNN_IMAGE_RESULT=UNSUPPORTED_MODEL_LAYOUT\n");
            return kExitLayoutFail;
        }
    }

    // ============================================================
    // Step 8: inputs_set (pass_through=0, UINT8, NHWC) — jilu.txt §7
    // ============================================================
    rknn_input input{};
    input.index        = 0;
    input.buf          = inputBuf.data();
    input.size         = static_cast<uint32_t>(inputBuf.size());
    input.pass_through = 0;
    input.type         = RKNN_TENSOR_UINT8;
    input.fmt          = RKNN_TENSOR_NHWC;

    auto tInf0 = std::chrono::steady_clock::now();
    int rcIn = rt.inputsSet(1, &input);
    std::printf("INPUTS_SET_RC=%d\n", rcIn);
    if (rcIn != RKNN_SUCC) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=inputs_set failed: rc=%d\n", rcIn);
        return kExitInputsSetFail;
    }

    // ============================================================
    // Step 9: run + outputs_get (want_float=1) — jilu.txt §7
    // ============================================================
    int rcRun = rt.run();
    std::printf("RUN_RC=%d\n", rcRun);
    if (rcRun != RKNN_SUCC) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=run failed: rc=%d\n", rcRun);
        return kExitRunFail;
    }

    std::vector<rknn_output> outputs(ioNum.n_output);
    for (uint32_t i = 0; i < ioNum.n_output; ++i) {
        outputs[i].want_float  = 1;   // ask runtime to convert FLOAT16 -> FLOAT32
        outputs[i].is_prealloc = 0;
        outputs[i].index       = i;
        outputs[i].buf         = nullptr;
        outputs[i].size        = 0;
    }
    // Print the caller's output request (jilu.txt §7).
    std::printf("CALLER_OUTPUT_REQUEST=FLOAT32\n");
    // Also print the model's native output type (FLOAT16 by default).
    std::printf("MODEL_OUTPUT_TYPE=%s\n",
                tensorTypeStr(outAttrs[0].type));

    int rcGet = rt.outputsGet(ioNum.n_output, outputs.data());
    std::printf("OUTPUTS_GET_RC=%d\n", rcGet);
    if (rcGet != RKNN_SUCC) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=outputs_get failed: rc=%d\n", rcGet);
        return kExitOutputsFail;
    }
    OutputsLease outputLease(rt, outputs);

    auto tInf1 = std::chrono::steady_clock::now();
    long infUs = std::chrono::duration_cast<std::chrono::microseconds>(
                     tInf1 - tInf0).count();
    std::printf("INFERENCE_TIME_US=%ld\n", infUs);

    // ============================================================
    // Step 10: decode all output layers through the shared postprocessor
    // (decode + class-aware NMS + reverse letterbox, jilu.txt §8)
    // ============================================================
    auto tPost0 = std::chrono::steady_clock::now();

    std::vector<rknn_image::OutputTensorView> outputViews;
    outputViews.reserve(ioNum.n_output);
    std::string decodeErr;

    for (uint32_t i = 0; i < ioNum.n_output; ++i) {
        if (outputs[i].buf == nullptr || outputs[i].size == 0) {
            decodeErr = "output[" + std::to_string(i) + "] null/empty buf";
            std::printf("RKNN_IMAGE_RESULT=FAIL\n");
            std::printf("ERROR=shared postprocess input: %s\n", decodeErr.c_str());
            return kExitInternalFail;
        }
        // Validate runtime gave us at least n_elems * sizeof(float) bytes.
        size_t needBytes = static_cast<size_t>(outAttrs[i].n_elems) * sizeof(float);
        if (outputs[i].size < needBytes) {
            decodeErr = "output[" + std::to_string(i) +
                        "] size " + std::to_string(outputs[i].size) +
                        " < needed " + std::to_string(needBytes);
            std::printf("RKNN_IMAGE_RESULT=FAIL\n");
            std::printf("ERROR=shared postprocess input: %s\n", decodeErr.c_str());
            return kExitInternalFail;
        }

        int gridW  = static_cast<int>(outAttrs[i].dims[3]);
        int gridH  = static_cast<int>(outAttrs[i].dims[2]);
        const std::size_t expectedElems =
            static_cast<std::size_t>(gridW) * static_cast<std::size_t>(gridH) *
            static_cast<std::size_t>(rknn_image::kChannelCount);
        if (static_cast<std::size_t>(outAttrs[i].n_elems) != expectedElems) {
            decodeErr = "output[" + std::to_string(i) + "] n_elems " +
                        std::to_string(outAttrs[i].n_elems) + " != expected " +
                        std::to_string(expectedElems);
            std::printf("RKNN_IMAGE_RESULT=FAIL\n");
            std::printf("ERROR=shared postprocess input: %s\n", decodeErr.c_str());
            return kExitInternalFail;
        }

        // Diagnostic: print raw output value statistics to detect whether
        // the model has built-in sigmoid (values in [0,1]) or outputs raw
        // logits (values may be negative or >1).
        {
            const float *raw = static_cast<const float *>(outputs[i].buf);
            int n = static_cast<int>(outAttrs[i].n_elems);
            if (n > 0 && raw) {
                float vmin = raw[0], vmax = raw[0], vsum = 0.0f;
                for (int k = 0; k < n; ++k) {
                    float v = raw[k];
                    if (v < vmin) vmin = v;
                    if (v > vmax) vmax = v;
                    vsum += v;
                }
                std::printf("LAYER_%u_RAW_STATS: n=%d min=%.4f max=%.4f mean=%.4f"
                            " first5=[%.4f %.4f %.4f %.4f %.4f]\n",
                            i, n, vmin, vmax, vsum / n,
                            raw[0], raw[1], raw[2], raw[3], raw[4]);
            }
        }

        rknn_image::OutputTensorView view;
        view.data = static_cast<const float *>(outputs[i].buf);
        view.byteSize = static_cast<std::size_t>(outputs[i].size);
        view.gridWidth = gridW;
        view.gridHeight = gridH;
        view.outputIndex = static_cast<int>(outputs[i].index);
        outputViews.push_back(view);
    }

    std::vector<rknn_image::Detection> kept;
    if (!rknn_image::decodeOutputs(outputViews, meta, lb,
                                   args.confThreshold, args.nmsThreshold,
                                   kept, decodeErr)) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=shared decodeOutputs: %s\n", decodeErr.c_str());
        return kExitInternalFail;
    }

    std::printf("POST_NMS_COUNT=%zu\n", kept.size());

    // ============================================================
    // Step 11: fill class names on shared decoded detections (jilu.txt §8)
    // ============================================================
    for (auto &d : kept) {
        if (d.classIndex >= 0 && d.classIndex < static_cast<int>(labels.size())) {
            d.className = labels[d.classIndex];
        }
    }
    std::printf("DETECTION_COUNT=%zu\n", kept.size());

    // ============================================================
    // Step 12: expect-class validation (jilu.txt §TDD item 14)
    // ============================================================
    bool expectFound = rknn_image::findExpectedClass(kept, expectClassIndex);
    std::printf("EXPECTED_CLASS_FOUND=%s\n", expectFound ? "true" : "false");

    auto tPost1 = std::chrono::steady_clock::now();
    long postUs = std::chrono::duration_cast<std::chrono::microseconds>(
                      tPost1 - tPost0).count();
    std::printf("POSTPROCESS_TIME_US=%ld\n", postUs);

    // ============================================================
    // Step 14: draw annotated image (jilu.txt §9)
    // ============================================================
    QImage annotated = rgb.copy();
    {
        QPainter p(&annotated);
        p.setRenderHint(QPainter::Antialiasing);
        for (const auto &d : kept) {
            drawDetection(p, d, srcW, srcH);
        }
    }
    QString outPath = QString::fromStdString(args.outputPath);
    // Ensure parent directory exists.
    QFileInfo fi(outPath);
    QDir().mkpath(fi.absolutePath());
    bool saved = annotated.save(outPath, "JPEG", 90);
    if (!saved) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=annotated image save failed: %s\n",
                    args.outputPath.c_str());
        return kExitInternalFail;
    }
    QFileInfo outInfo(outPath);
    qint64 outSize = outInfo.size();
    std::printf("ANNOTATED_PATH=%s\n", args.outputPath.c_str());
    std::printf("ANNOTATED_SIZE=%lld\n", static_cast<long long>(outSize));

    // ============================================================
    // Step 15: release outputs (always, jilu.txt §7)
    // ============================================================
    int rcRel = outputLease.release();
    if (rcRel != RKNN_SUCC) {
        std::printf("RKNN_IMAGE_RESULT=FAIL\n");
        std::printf("ERROR=outputs_release failed: rc=%d\n", rcRel);
        return kExitOutputsFail;
    }

    // ============================================================
    // Step 16: final verdict (jilu.txt §验收)
    // ============================================================
    // PASS requires:
    //   - at least one valid detection
    //   - expect-class found
    //   - annotated image non-empty
    //   - no NaN/Inf/越界 (already checked in decode)
    bool pass = !kept.empty() && expectFound && outSize > 0;
    std::printf("RKNN_IMAGE_RESULT=%s\n", pass ? "PASS" : "FAIL");
    std::fflush(stdout);
    return pass ? kExitOk : kExitInternalFail;
}
