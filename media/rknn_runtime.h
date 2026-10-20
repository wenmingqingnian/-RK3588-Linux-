#ifndef RKNN_RUNTIME_H
#define RKNN_RUNTIME_H

// Qt-free, dependency-free RAII wrapper around the official RKNN C API.
//
// Design goals (per jilu.txt §四):
//   - Includes the OFFICIAL <rknn_api.h> only. No hand-written ABI structs.
//   - Function pointer types derived from official declarations via decltype(&::fn).
//   - dlopen/dlsym with per-symbol error reporting.
//   - rknn_init uses the 5-parameter signature (extend = nullptr).
//   - Explicit rknn_run() — never assume outputs_get runs the model.
//   - rknn_outputs_get passes nullptr for rknn_output_extend*.
//   - rknn_context is an integer handle on aarch64 — initialized to 0, NEVER nullptr.
//   - Only depends on C++17, dl, and the official rknn_api.h.
//   - No Qt, no OpenCV, no RGA, no MPP, no RKLLM, no ALSA, no camera.
//
// This header is only compilable where /usr/local/include/rknn_api.h (or
// equivalent) is available. Callers without the official header must use
// the disabled-stub path in rknn_detector.cpp.

#include <rknn_api.h>

#include <cstdint>
#include <string>

namespace rknn_runtime {

// Function pointer types derived from the official rknn_api.h declarations.
// Per jilu.txt §四.3, hand-writing prototypes is forbidden.
using RknnInitFn           = decltype(&::rknn_init);
using RknnDestroyFn        = decltype(&::rknn_destroy);
using RknnQueryFn          = decltype(&::rknn_query);
using RknnInputsSetFn      = decltype(&::rknn_inputs_set);
using RknnRunFn            = decltype(&::rknn_run);
using RknnOutputsGetFn     = decltype(&::rknn_outputs_get);
using RknnOutputsReleaseFn = decltype(&::rknn_outputs_release);

// RAII wrapper around a single RKNN context.
//
// Lifecycle:
//   1. loadLibrary() — dlopen librknnrt.so and resolve all symbols.
//   2. initModel()   — read model file, call rknn_init(... nullptr).
//   3. query*()      — query SDK version, IO count, tensor attributes.
//   4. inputsSet/run/outputsGet/outputsRelease — inference loop.
//   5. Destructor    — rknn_destroy + dlclose (idempotent).
//
// Not thread-safe: each thread must use its own instance (per jilu.txt §P1-2).
class Runtime {
public:
    Runtime();
    ~Runtime();

    // Non-copyable, non-movable (owns dlopen handle + rknn_context).
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;
    Runtime(Runtime &&) = delete;
    Runtime &operator=(Runtime &&) = delete;

    // Step 1: dlopen librknnrt.so and resolve the 7 required symbols.
    // Returns false and sets lastError() on any failure.
    // `libraryPath` may be a bare soname ("librknnrt.so") or a full path.
    bool loadLibrary(const std::string &libraryPath);

    // Step 2: read model file fully into memory, then call
    //   rknn_init(&ctx, modelData, modelSize, 0 /*flag*/, nullptr /*extend*/).
    // The model buffer is retained until destroy() per jilu.txt §四.5
    // (do not assume rknn_init copies the model).
    // Returns true on RKNN_SUCC; otherwise sets lastError() and returns false.
    bool initModel(const std::string &modelPath);

    // Query helpers — all require a valid context from initModel().
    bool querySdkVersion(rknn_sdk_version &outVersion);
    bool queryIoNum(rknn_input_output_num &outNum);
    bool queryInputAttr(uint32_t index, rknn_tensor_attr &outAttr);
    bool queryOutputAttr(uint32_t index, rknn_tensor_attr &outAttr);

    // Inference primitives — thin typed wrappers around the resolved symbols.
    // Each returns the raw RKNN return code (RKNN_SUCC == 0 on success).
    int  inputsSet(uint32_t nInputs, rknn_input *inputs);
    int  run();                       // rknn_run(ctx, nullptr)
    int  outputsGet(uint32_t nOutputs, rknn_output *outputs);   // 4th arg = nullptr
    int  outputsRelease(uint32_t nOutputs, rknn_output *outputs);

    // Accessors.
    bool        isReady() const { return m_contextValid; }
    bool        hasLibrary() const { return m_libHandle != nullptr; }
    rknn_context context() const { return m_context; }
    const std::string &lastError() const { return m_lastError; }

    // Explicit teardown (also called by destructor). Idempotent.
    void deinit();

private:
    void *m_libHandle;
    rknn_context m_context;          // integer handle — value 0 means invalid
    bool m_contextValid;
    std::string m_lastError;

    // Model buffer retained for context lifetime (jilu.txt §四.5: do not
    // assume rknn_init copies the model). Freed in deinit().
    void *m_modelBuffer;
    uint32_t m_modelSize;

    // Resolved function pointers (nullptr until loadLibrary succeeds).
    RknnInitFn           m_fnInit;
    RknnDestroyFn        m_fnDestroy;
    RknnQueryFn          m_fnQuery;
    RknnInputsSetFn      m_fnInputsSet;
    RknnRunFn            m_fnRun;
    RknnOutputsGetFn     m_fnOutputsGet;
    RknnOutputsReleaseFn m_fnOutputsRelease;
};

} // namespace rknn_runtime

#endif // RKNN_RUNTIME_H
