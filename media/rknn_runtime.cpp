#include "rknn_runtime.h"

#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cerrno>

namespace rknn_runtime {

static void setError(std::string &dst, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    dst = buf;
}

Runtime::Runtime()
    : m_libHandle(nullptr),
      m_context(0),                  // integer handle — 0 means invalid (NEVER nullptr)
      m_contextValid(false),
      m_modelBuffer(nullptr),
      m_modelSize(0),
      m_fnInit(nullptr),
      m_fnDestroy(nullptr),
      m_fnQuery(nullptr),
      m_fnInputsSet(nullptr),
      m_fnRun(nullptr),
      m_fnOutputsGet(nullptr),
      m_fnOutputsRelease(nullptr)
{
}

Runtime::~Runtime()
{
    deinit();
}

bool Runtime::loadLibrary(const std::string &libraryPath)
{
    if (m_libHandle) {
        setError(m_lastError, "loadLibrary called twice; use deinit first");
        return false;
    }
    if (libraryPath.empty()) {
        setError(m_lastError, "empty library path");
        return false;
    }

    // RTLD_NOW | RTLD_LOCAL per jilu.txt §四.2.
    m_libHandle = dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m_libHandle) {
        setError(m_lastError, "dlopen(%s) failed: %s", libraryPath.c_str(), dlerror());
        return false;
    }

    // Clear any stale dlerror state.
    dlerror();

    // Resolve each required symbol with per-symbol error reporting (§四.3).
    struct SymEntry { const char *name; void **target; };
    const SymEntry required[] = {
        { "rknn_init",           reinterpret_cast<void **>(&m_fnInit) },
        { "rknn_destroy",        reinterpret_cast<void **>(&m_fnDestroy) },
        { "rknn_query",          reinterpret_cast<void **>(&m_fnQuery) },
        { "rknn_inputs_set",     reinterpret_cast<void **>(&m_fnInputsSet) },
        { "rknn_run",            reinterpret_cast<void **>(&m_fnRun) },
        { "rknn_outputs_get",    reinterpret_cast<void **>(&m_fnOutputsGet) },
        { "rknn_outputs_release",reinterpret_cast<void **>(&m_fnOutputsRelease) },
    };

    std::string missing;
    for (const auto &e : required) {
        *e.target = dlsym(m_libHandle, e.name);
        const char *err = dlerror();
        if (err || !*e.target) {
            if (!missing.empty()) missing += ", ";
            missing += e.name;
        }
    }

    if (!missing.empty()) {
        setError(m_lastError, "missing RKNN symbols in %s: %s",
                 libraryPath.c_str(), missing.c_str());
        dlclose(m_libHandle);
        m_libHandle = nullptr;
        // Null all function pointers (defensive).
        m_fnInit = nullptr; m_fnDestroy = nullptr; m_fnQuery = nullptr;
        m_fnInputsSet = nullptr; m_fnRun = nullptr;
        m_fnOutputsGet = nullptr; m_fnOutputsRelease = nullptr;
        return false;
    }

    return true;
}

bool Runtime::initModel(const std::string &modelPath)
{
    if (!m_libHandle || !m_fnInit) {
        setError(m_lastError, "initModel called before successful loadLibrary");
        return false;
    }
    if (m_contextValid) {
        setError(m_lastError, "initModel called twice; call deinit first");
        return false;
    }
    if (modelPath.empty()) {
        setError(m_lastError, "empty model path");
        return false;
    }

    // --- Model file read with full protection (jilu.txt §四.5) ---
    FILE *fp = std::fopen(modelPath.c_str(), "rb");
    if (!fp) {
        setError(m_lastError, "fopen(%s) failed: %s", modelPath.c_str(), std::strerror(errno));
        return false;
    }

    if (std::fseek(fp, 0, SEEK_END) != 0) {
        setError(m_lastError, "fseek_end failed on %s", modelPath.c_str());
        std::fclose(fp);
        return false;
    }
    long fileSize = std::ftell(fp);
    if (fileSize <= 0) {
        setError(m_lastError, "model file empty or ftell failed: %s (size=%ld)",
                 modelPath.c_str(), fileSize);
        std::fclose(fp);
        return false;
    }
    if (std::fseek(fp, 0, SEEK_SET) != 0) {
        setError(m_lastError, "fseek_set failed on %s", modelPath.c_str());
        std::fclose(fp);
        return false;
    }

    // Guard against overflow of uint32_t size parameter (§四.5).
    if (static_cast<unsigned long long>(fileSize) > 0xFFFFFFFFULL) {
        setError(m_lastError, "model file too large for uint32_t size: %ld", fileSize);
        std::fclose(fp);
        return false;
    }

    uint32_t modelSize = static_cast<uint32_t>(fileSize);

    // Use malloc (not new[]) so we can hand the buffer to rknn_init without
    // worrying about element type; free() is called only in deinit().
    void *modelData = std::malloc(modelSize);
    if (!modelData) {
        setError(m_lastError, "malloc(%u) failed for model buffer", modelSize);
        std::fclose(fp);
        return false;
    }

    size_t readBack = std::fread(modelData, 1, modelSize, fp);
    std::fclose(fp);
    if (readBack != modelSize) {
        setError(m_lastError, "model read truncated: %llu / %u bytes",
                 static_cast<unsigned long long>(readBack), modelSize);
        std::free(modelData);
        return false;
    }

    // --- rknn_init with 5-parameter signature (§四.6, §P0-2) ---
    // rknn_context is an integer handle on aarch64 — already zero-initialized.
    int ret = m_fnInit(&m_context, modelData, modelSize, 0 /*flag*/, nullptr /*extend*/);
    if (ret != RKNN_SUCC) {
        setError(m_lastError, "rknn_init failed: ret=%d (size=%u)", ret, modelSize);
        std::free(modelData);
        m_context = 0;
        return false;
    }

    m_contextValid = true;
    // Per jilu.txt §四.5 the model buffer must remain valid for the context
    // lifetime (we cannot assume rknn_init copies it). Retain ownership and
    // free it in deinit().
    m_modelBuffer = modelData;
    m_modelSize = modelSize;
    return true;
}

bool Runtime::querySdkVersion(rknn_sdk_version &outVersion)
{
    if (!m_contextValid) {
        setError(m_lastError, "querySdkVersion called without valid context");
        return false;
    }
    std::memset(&outVersion, 0, sizeof(outVersion));
    int ret = m_fnQuery(m_context, RKNN_QUERY_SDK_VERSION,
                        &outVersion, sizeof(outVersion));
    if (ret != RKNN_SUCC) {
        setError(m_lastError, "rknn_query(SDK_VERSION) failed: ret=%d", ret);
        return false;
    }
    return true;
}

bool Runtime::queryIoNum(rknn_input_output_num &outNum)
{
    if (!m_contextValid) {
        setError(m_lastError, "queryIoNum called without valid context");
        return false;
    }
    std::memset(&outNum, 0, sizeof(outNum));
    int ret = m_fnQuery(m_context, RKNN_QUERY_IN_OUT_NUM,
                        &outNum, sizeof(outNum));
    if (ret != RKNN_SUCC) {
        setError(m_lastError, "rknn_query(IN_OUT_NUM) failed: ret=%d", ret);
        return false;
    }
    return true;
}

bool Runtime::queryInputAttr(uint32_t index, rknn_tensor_attr &outAttr)
{
    if (!m_contextValid) {
        setError(m_lastError, "queryInputAttr called without valid context");
        return false;
    }
    std::memset(&outAttr, 0, sizeof(outAttr));
    outAttr.index = index;
    int ret = m_fnQuery(m_context, RKNN_QUERY_INPUT_ATTR,
                        &outAttr, sizeof(outAttr));
    if (ret != RKNN_SUCC) {
        setError(m_lastError, "rknn_query(INPUT_ATTR[%u]) failed: ret=%d", index, ret);
        return false;
    }
    return true;
}

bool Runtime::queryOutputAttr(uint32_t index, rknn_tensor_attr &outAttr)
{
    if (!m_contextValid) {
        setError(m_lastError, "queryOutputAttr called without valid context");
        return false;
    }
    std::memset(&outAttr, 0, sizeof(outAttr));
    outAttr.index = index;
    int ret = m_fnQuery(m_context, RKNN_QUERY_OUTPUT_ATTR,
                        &outAttr, sizeof(outAttr));
    if (ret != RKNN_SUCC) {
        setError(m_lastError, "rknn_query(OUTPUT_ATTR[%u]) failed: ret=%d", index, ret);
        return false;
    }
    return true;
}

int Runtime::inputsSet(uint32_t nInputs, rknn_input *inputs)
{
    if (!m_contextValid) return RKNN_ERR_CTX_INVALID;
    return m_fnInputsSet(m_context, nInputs, inputs);
}

int Runtime::run()
{
    if (!m_contextValid) return RKNN_ERR_CTX_INVALID;
    // rknn_run(ctx, nullptr) — synchronous blocking inference (§四.13, §P0-3).
    return m_fnRun(m_context, nullptr);
}

int Runtime::outputsGet(uint32_t nOutputs, rknn_output *outputs)
{
    if (!m_contextValid) return RKNN_ERR_CTX_INVALID;
    // 4th argument is rknn_output_extend* — nullptr for plain sync path (§P0-4).
    return m_fnOutputsGet(m_context, nOutputs, outputs, nullptr);
}

int Runtime::outputsRelease(uint32_t nOutputs, rknn_output *outputs)
{
    if (!m_contextValid) return RKNN_ERR_CTX_INVALID;
    return m_fnOutputsRelease(m_context, nOutputs, outputs);
}

void Runtime::deinit()
{
    // Idempotent (§四.16): safe to call multiple times.
    if (m_contextValid && m_fnDestroy) {
        m_fnDestroy(m_context);
    }
    m_context = 0;
    m_contextValid = false;

    if (m_modelBuffer) {
        std::free(m_modelBuffer);
        m_modelBuffer = nullptr;
        m_modelSize = 0;
    }

    if (m_libHandle) {
        dlclose(m_libHandle);
        m_libHandle = nullptr;
    }

    m_fnInit = nullptr;
    m_fnDestroy = nullptr;
    m_fnQuery = nullptr;
    m_fnInputsSet = nullptr;
    m_fnRun = nullptr;
    m_fnOutputsGet = nullptr;
    m_fnOutputsRelease = nullptr;
}

} // namespace rknn_runtime
