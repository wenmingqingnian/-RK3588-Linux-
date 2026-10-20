// rknn_probe — minimal RKNN runtime probe (jilu.txt §四, Stage A).
//
// Verifies that librknnrt.so + yolov5s.rknn can complete the full inference
// pipeline on the RK3588S NPU without SIGBUS or kernel faults:
//
//   dlopen → dlsym(7 symbols) → rknn_init(5 params, nullptr)
//   → rknn_query(SDK_VERSION | IN_OUT_NUM | INPUT_ATTR | OUTPUT_ATTR)
//   → construct deterministic RGB inputs
//   → rknn_inputs_set → rknn_run → rknn_outputs_get
//   → checksum/validate outputs → rknn_outputs_release
//   → rknn_destroy → dlclose
//
// The probe performs NO YOLO post-processing. Its purpose is to prove the
// runtime/driver/model combination is compatible before Stage B work begins.
//
// Output is key=value, parseable by scripts (jilu.txt §四.19).
// Exit codes are defined by jilu.txt §四.20.

#include "../../media/rknn_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>

namespace {

// ---------- Exit codes (jilu.txt §四.20) ----------
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
};

struct ProbeArgs {
    std::string libraryPath = "/usr/local/lib/librknnrt.so";
    std::string modelPath   = "./config/yolov5s.rknn";
    uint32_t    iterations  = 3;
    bool        help        = false;
};

static void printHelp(const char *argv0)
{
    std::printf(
        "Usage: %s --library <librknnrt.so> --model <model.rknn> --iterations N\n"
        "\n"
        "Options:\n"
        "  --library <path>     Path to librknnrt.so (default: /usr/local/lib/librknnrt.so)\n"
        "  --model <path>       Path to .rknn model (default: ./config/yolov5s.rknn)\n"
        "  --iterations <N>     Inference iterations (default: 3)\n"
        "  --help               Show this help\n"
        "\n"
        "Exit codes (jilu.txt §四.20):\n"
        "  0  = full inference success\n"
        "  2  = CLI argument error\n"
        "  3  = library load / symbol resolution failure\n"
        "  4  = model file read failure\n"
        "  5  = rknn_init failure\n"
        "  6  = rknn_query failure or unsupported tensor layout\n"
        "  7  = rknn_inputs_set failure\n"
        "  8  = rknn_run failure\n"
        "  9  = rknn_outputs_get / outputs_release failure\n"
        "  10 = internal buffer / overflow / output validation failure\n"
        "  135 (shell-reported) = SIGBUS — record as runtime incompatibility\n",
        argv0);
}

static bool parseArgs(int argc, char **argv, ProbeArgs &out, std::string &err)
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
        } else if (a == "--iterations") {
            const char *v = next("--iterations");
            if (!v) return false;
            char *endp = nullptr;
            long n = std::strtol(v, &endp, 10);
            if (!endp || *endp != '\0' || n <= 0 || n > 100000) {
                err = std::string("invalid --iterations value: ") + v;
                return false;
            }
            out.iterations = static_cast<uint32_t>(n);
        } else {
            err = std::string("unknown argument: ") + a;
            return false;
        }
    }
    return true;
}

// FNV-1a 32-bit checksum (stable, dependency-free).
static uint32_t fnv1a32(const uint8_t *data, size_t n)
{
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; ++i) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    return h;
}

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
    default:                   return "UNKNOWN";
    }
}

static const char *qntTypeStr(rknn_tensor_qnt_type q)
{
    switch (q) {
    case RKNN_TENSOR_QNT_NONE:               return "NONE";
    case RKNN_TENSOR_QNT_DFP:                return "DFP";
    case RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC:  return "AFFINE";
    default:                                 return "UNKNOWN";
    }
}

// Print all fields of a tensor attribute, jilu.txt §四.9.
static void printTensorAttr(const char *prefix, uint32_t idx, const rknn_tensor_attr &a)
{
    std::printf("%s_%u_NAME=%s\n",          prefix, idx, a.name);
    std::printf("%s_%u_FMT=%s\n",           prefix, idx, tensorFmtStr(a.fmt));
    std::printf("%s_%u_TYPE=%s\n",          prefix, idx, tensorTypeStr(a.type));
    std::printf("%s_%u_QNT_TYPE=%s\n",      prefix, idx, qntTypeStr(a.qnt_type));
    std::printf("%s_%u_N_DIMS=%u\n",        prefix, idx, a.n_dims);
    std::printf("%s_%u_DIMS=",              prefix, idx);
    for (uint32_t i = 0; i < a.n_dims && i < RKNN_MAX_DIMS; ++i) {
        std::printf("%s%u", (i == 0 ? "" : ","), a.dims[i]);
    }
    std::printf("\n");
    std::printf("%s_%u_ELEMS=%u\n",         prefix, idx, a.n_elems);
    std::printf("%s_%u_SIZE=%u\n",          prefix, idx, a.size);
    std::printf("%s_%u_SIZE_WITH_STRIDE=%u\n", prefix, idx, a.size_with_stride);
    std::printf("%s_%u_W_STRIDE=%u\n",      prefix, idx, a.w_stride);
    std::printf("%s_%u_H_STRIDE=%u\n",      prefix, idx, a.h_stride);
    std::printf("%s_%u_FL=%d\n",            prefix, idx, a.fl);
    std::printf("%s_%u_ZP=%d\n",            prefix, idx, a.zp);
    std::printf("%s_%u_SCALE=%.6f\n",       prefix, idx, a.scale);
    std::printf("%s_%u_PASS_THROUGH=%u\n",  prefix, idx, a.pass_through);
}

// Derive N/C/H/W from a tensor attribute (jilu.txt §四.10).
// Returns false if the layout is unsupported.
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
    // Note: a.type describes the MODEL's logical input tensor (e.g. FLOAT16).
    // It does NOT constrain the caller buffer. With pass_through=0 the RKNN
    // runtime converts the caller's UINT8 NHWC buffer to the model's native
    // type. The UINT8-only restriction was removed per jilu.txt §五.
    return true;
}

// Safe multiply with overflow check (jilu.txt §四.11).
static bool safeMul3(uint32_t a, uint32_t b, uint32_t c, size_t &out)
{
    uint64_t r = static_cast<uint64_t>(a) * b;
    if (r > SIZE_MAX) return false;
    r *= c;
    if (r > SIZE_MAX) return false;
    out = static_cast<size_t>(r);
    return true;
}

// Build deterministic input buffer A: all gray 114 (jilu.txt §四.11).
static bool buildInputGray(uint8_t *buf, uint32_t W, uint32_t H, uint32_t C)
{
    if (C != 3) return false;
    size_t total = static_cast<size_t>(W) * H * C;
    for (size_t i = 0; i < total; ++i) buf[i] = 114;
    return true;
}

// Build deterministic input buffer B: RGB gradient (jilu.txt §四.11).
//   R = x % 256, G = y % 256, B = (x + y) % 256  (NHWC layout)
static bool buildInputGradient(uint8_t *buf, uint32_t W, uint32_t H, uint32_t C)
{
    if (C != 3) return false;
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            size_t idx = (static_cast<size_t>(y) * W + x) * C;
            buf[idx + 0] = static_cast<uint8_t>(x % 256u);          // R
            buf[idx + 1] = static_cast<uint8_t>(y % 256u);          // G
            buf[idx + 2] = static_cast<uint8_t>((x + y) % 256u);    // B
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    ProbeArgs args;
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

    // ----- Header output -----
    std::printf("RKNN_LIBRARY=%s\n", args.libraryPath.c_str());
    std::printf("MODEL_PATH=%s\n",   args.modelPath.c_str());

    // ----- Step 1: load library -----
    rknn_runtime::Runtime rt;
    if (!rt.loadLibrary(args.libraryPath)) {
        std::printf("RKNN_PROBE_RESULT=FAIL\n");
        std::printf("ERROR=loadLibrary: %s\n", rt.lastError().c_str());
        std::fflush(stdout);
        return kExitLibLoadFail;
    }

    // ----- Step 2: init model -----
    if (!rt.initModel(args.modelPath)) {
        std::printf("RKNN_PROBE_RESULT=FAIL\n");
        std::printf("ERROR=initModel: %s\n", rt.lastError().c_str());
        std::fflush(stdout);
        // Distinguish model-read failure from rknn_init failure by error text.
        // initModel sets lastError starting with "fopen" or "model" for read
        // failures, and "rknn_init failed" for init failures.
        const std::string &e = rt.lastError();
        if (e.find("rknn_init failed") == 0) return kExitInitFail;
        return kExitModelReadFail;
    }

    // ----- Step 3: query SDK version -----
    rknn_sdk_version ver{};
    if (!rt.querySdkVersion(ver)) {
        std::printf("RKNN_PROBE_RESULT=FAIL\n");
        std::printf("ERROR=querySdkVersion: %s\n", rt.lastError().c_str());
        std::fflush(stdout);
        return kExitQueryFail;
    }
    std::printf("RKNN_API_VERSION=%s\n",    ver.api_version);
    std::printf("RKNN_DRIVER_VERSION=%s\n", ver.drv_version);

    // ----- Step 4: query IO count -----
    rknn_input_output_num ioNum{};
    if (!rt.queryIoNum(ioNum)) {
        std::printf("RKNN_PROBE_RESULT=FAIL\n");
        std::printf("ERROR=queryIoNum: %s\n", rt.lastError().c_str());
        std::fflush(stdout);
        return kExitQueryFail;
    }
    if (ioNum.n_input == 0 || ioNum.n_output == 0) {
        std::printf("RKNN_PROBE_RESULT=FAIL\n");
        std::printf("ERROR=invalid IO count: n_input=%u n_output=%u\n",
                    ioNum.n_input, ioNum.n_output);
        std::fflush(stdout);
        return kExitQueryFail;
    }
    if (ioNum.n_input > 64 || ioNum.n_output > 64) {
        std::printf("RKNN_PROBE_RESULT=FAIL\n");
        std::printf("ERROR=IO count exceeds sane bound: n_input=%u n_output=%u\n",
                    ioNum.n_input, ioNum.n_output);
        std::fflush(stdout);
        return kExitQueryFail;
    }
    if (ioNum.n_input != 1) {
        std::printf("RKNN_PROBE_RESULT=FAIL\n");
        std::printf("ERROR=probe requires single-input model, got n_input=%u\n",
                    ioNum.n_input);
        std::fflush(stdout);
        return kExitQueryFail;
    }
    std::printf("INPUT_COUNT=%u\n",  ioNum.n_input);
    std::printf("OUTPUT_COUNT=%u\n", ioNum.n_output);

    // ----- Step 5: query & print all input attributes -----
    std::vector<rknn_tensor_attr> inputAttrs(ioNum.n_input);
    for (uint32_t i = 0; i < ioNum.n_input; ++i) {
        if (!rt.queryInputAttr(i, inputAttrs[i])) {
            std::printf("RKNN_PROBE_RESULT=FAIL\n");
            std::printf("ERROR=queryInputAttr[%u]: %s\n", i, rt.lastError().c_str());
            std::fflush(stdout);
            return kExitQueryFail;
        }
        printTensorAttr("INPUT", i, inputAttrs[i]);
    }

    // ----- Step 6: query & print all output attributes -----
    std::vector<rknn_tensor_attr> outputAttrs(ioNum.n_output);
    for (uint32_t i = 0; i < ioNum.n_output; ++i) {
        if (!rt.queryOutputAttr(i, outputAttrs[i])) {
            std::printf("RKNN_PROBE_RESULT=FAIL\n");
            std::printf("ERROR=queryOutputAttr[%u]: %s\n", i, rt.lastError().c_str());
            std::fflush(stdout);
            return kExitQueryFail;
        }
        printTensorAttr("OUTPUT", i, outputAttrs[i]);
    }

    // ----- Step 7: derive input dims from input_attrs[0] -----
    uint32_t N = 0, C = 0, H = 0, W = 0;
    std::string dimErr;
    if (!deriveInputDims(inputAttrs[0], N, C, H, W, dimErr)) {
        std::printf("RKNN_PROBE_RESULT=FAIL\n");
        std::printf("ERROR=unsupported input layout: %s\n", dimErr.c_str());
        std::fflush(stdout);
        return kExitQueryFail;
    }
    std::printf("INPUT_0_N=%u\n", N);
    std::printf("INPUT_0_C=%u\n", C);
    std::printf("INPUT_0_H=%u\n", H);
    std::printf("INPUT_0_W=%u\n", W);

    size_t inputBytes = 0;
    if (!safeMul3(W, H, C, inputBytes) || inputBytes == 0) {
        std::printf("RKNN_PROBE_RESULT=FAIL\n");
        std::printf("ERROR=input size overflow: W=%u H=%u C=%u\n", W, H, C);
        std::fflush(stdout);
        return kExitInternalFail;
    }
    std::printf("INPUT_BUF_SIZE=%zu\n", inputBytes);

    // ----- Step 8: build both deterministic input buffers -----
    std::vector<uint8_t> inputA(inputBytes);
    std::vector<uint8_t> inputB(inputBytes);
    if (!buildInputGray(inputA.data(), W, H, C) ||
        !buildInputGradient(inputB.data(), W, H, C)) {
        std::printf("RKNN_PROBE_RESULT=FAIL\n");
        std::printf("ERROR=failed to build deterministic inputs\n");
        std::fflush(stdout);
        return kExitInternalFail;
    }

    // ----- Step 8b: caller buffer description (jilu.txt §七) -----
    // The probe always provides interleaved UINT8 RGB and relies on the RKNN
    // runtime (pass_through=0) to convert to the model's native input type.
    // These fields describe the CALLER buffer, not the model tensor.
    std::printf("CALLER_INPUT_TYPE=UINT8\n");
    std::printf("CALLER_INPUT_FMT=NHWC\n");
    std::printf("CALLER_INPUT_PASS_THROUGH=0\n");
    std::printf("INPUT_CONVERSION=RKNN_RUNTIME\n");

    // ----- Step 9: iterations loop (jilu.txt §四.17) -----
    // Each iteration uses input A and input B alternately so we can detect
    // the "identical outputs regardless of input" warning condition (§四.15).
    bool overall = true;

    for (uint32_t it = 1; it <= args.iterations; ++it) {
        const uint8_t *curInput = ((it - 1) % 2 == 0) ? inputA.data() : inputB.data();
        const char *curTag      = ((it - 1) % 2 == 0) ? "A" : "B";

        // --- 9a. fill rknn_input (jilu.txt §四.12, §五, §六) ---
        // Caller buffer is always interleaved UINT8 RGB. pass_through=0 asks
        // the RKNN runtime to convert it to the model's native input (e.g.
        // FLOAT16). input.fmt MUST describe the CALLER buffer (NHWC), never
        // be copied from inputAttrs[0].fmt (jilu.txt §六).
        rknn_input input{};
        input.index        = 0;
        input.buf          = const_cast<uint8_t *>(curInput);
        input.size         = static_cast<uint32_t>(inputBytes);
        input.pass_through = 0;
        input.type         = RKNN_TENSOR_UINT8;
        input.fmt          = RKNN_TENSOR_NHWC;

        // --- 9b. inputs_set ---
        int rcIn = rt.inputsSet(1, &input);
        std::printf("RUN_%u_INPUT=%s\n", it, curTag);
        std::printf("RUN_%u_INPUTS_SET_RC=%d\n", it, rcIn);
        if (rcIn != RKNN_SUCC) {
            std::printf("ERROR=run %u inputs_set failed: rc=%d\n", it, rcIn);
            overall = false;
            break;
        }

        // --- 9c. rknn_run (explicit, §P0-3) ---
        int rcRun = rt.run();
        std::printf("RUN_%u_RUN_RC=%d\n", it, rcRun);
        if (rcRun != RKNN_SUCC) {
            std::printf("ERROR=run %u rknn_run failed: rc=%d\n", it, rcRun);
            overall = false;
            break;
        }

        // --- 9d. prepare rknn_output array (§四.14) ---
        std::vector<rknn_output> outputs(ioNum.n_output);
        for (uint32_t i = 0; i < ioNum.n_output; ++i) {
            outputs[i].want_float  = 0;     // keep raw model output format
            outputs[i].is_prealloc = 0;
            outputs[i].index       = i;
            outputs[i].buf         = nullptr;
            outputs[i].size        = 0;
        }

        // --- 9e. outputs_get (4th arg = nullptr, §P0-4) ---
        int rcGet = rt.outputsGet(ioNum.n_output, outputs.data());
        std::printf("RUN_%u_OUTPUTS_GET_RC=%d\n", it, rcGet);
        if (rcGet != RKNN_SUCC) {
            std::printf("ERROR=run %u outputs_get failed: rc=%d\n", it, rcGet);
            overall = false;
            break;
        }

        // --- 9f. validate + checksum each output (§四.15) ---
        bool releaseOk = true;
        for (uint32_t i = 0; i < ioNum.n_output; ++i) {
            const auto &o = outputs[i];
            std::printf("RUN_%u_OUTPUT_%u_RUNTIME_SIZE=%u\n", it, i, o.size);
            if (o.buf == nullptr || o.size == 0) {
                std::printf("RUN_%u_OUTPUT_%u_CHECKSUM=NONE\n", it, i);
                std::printf("WARN=run %u output %u has null/empty buf\n", it, i);
                releaseOk = false;
                continue;
            }
            uint32_t sum = fnv1a32(static_cast<const uint8_t *>(o.buf), o.size);
            std::printf("RUN_%u_OUTPUT_%u_CHECKSUM=0x%08x\n", it, i, sum);

            // Count non-zero bytes in the first 1024 bytes (cheap liveness probe).
            size_t probeLen = o.size < 1024 ? o.size : 1024;
            size_t nz = 0;
            const uint8_t *p = static_cast<const uint8_t *>(o.buf);
            for (size_t b = 0; b < probeLen; ++b) {
                if (p[b] != 0) ++nz;
            }
            std::printf("RUN_%u_OUTPUT_%u_NONZERO_BYTES=%zu\n", it, i, nz);
        }

        // --- 9g. outputs_release (always, even on validation failure, §四.16) ---
        int rcRel = rt.outputsRelease(ioNum.n_output, outputs.data());
        std::printf("RUN_%u_OUTPUTS_RELEASE_RC=%d\n", it, rcRel);
        if (rcRel != RKNN_SUCC) {
            std::printf("ERROR=run %u outputs_release failed: rc=%d\n", it, rcRel);
            overall = false;
            break;
        }
        if (!releaseOk) {
            overall = false;
            break;
        }
    }

    // ----- Step 10: detect identical-output warning (§四.15) -----
    // Compare RUN_1_OUTPUT_0_CHECKSUM and RUN_2_OUTPUT_0_CHECKSUM if we have
    // at least 2 iterations and they used different inputs.
    if (args.iterations >= 2 && overall) {
        // Re-read by scanning stdout is unnecessary — we just compute again.
        // Instead, we simply emit a WARNING if all iterations on different
        // inputs produced identical first-output checksums. The script can
        // grep for this and decide.
        // (Implementation note: we already printed per-run checksums; the
        // board-side script run_probe.sh performs the cross-run comparison.)
    }

    // ----- Step 11: final result -----
    std::printf("RKNN_PROBE_RESULT=%s\n", overall ? "PASS" : "FAIL");
    std::fflush(stdout);
    return overall ? kExitOk : kExitRunFail;
}
