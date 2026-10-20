/**
 * @file KwsEngine.cpp
 * @brief Offline keyword spotter implementation using sherpa-onnx via dlopen.
 *
 * Mirrors AsrEngine's dlopen/dlsym/ABI-verify pattern. No compile-time
 * dependency on sherpa-onnx headers.
 *
 * Key differences from AsrEngine:
 *   - Creates SherpaOnnxKeywordSpotter instead of OnlineRecognizer
 *   - No endpoint detection; runs continuously
 *   - getResult() reads KeywordResult->keyword (not OnlineRecognizerResult->text)
 *   - reset() MUST be called after a hit (SherpaOnnxResetKeywordStream)
 *
 * Reference: c-api-examples/kws-c-api.c (sherpa-onnx v1.13.3)
 */

#include "KwsEngine.h"
#include "logmanager.h"
#include "sherpa_onnx_abi_1_13_3.h"

#include <dlfcn.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>

static const char *MODULE = "KwsEngine";

// ---------------------------------------------------------------------------
// sherpa-onnx C API struct definitions for config construction only.
//
// These must match the layout expected by the installed sherpa-onnx library.
// If the library version changes, these definitions must be updated.
//
// Note: SherpaOnnxKeywordSpotterConfig reuses SherpaOnnxFeatureConfig and
// SherpaOnnxOnlineModelConfig from the OnlineRecognizer API (same structs).
// We redefine them here file-locally to keep KwsEngine self-contained
// (AsrEngine.cpp also defines them file-locally; the layouts are identical).
// ---------------------------------------------------------------------------

struct SherpaOnnxOnlineTransducerModelConfig {
    const char *encoder;
    const char *decoder;
    const char *joiner;
};

struct SherpaOnnxOnlineParaformerModelConfig {
    const char *encoder;
    const char *decoder;
};

struct SherpaOnnxOnlineZipformer2CtcModelConfig {
    const char *model;
};

struct SherpaOnnxOnlineNemoCtcModelConfig {
    const char *model;
};

struct SherpaOnnxOnlineToneCtcModelConfig {
    const char *model;
};

struct SherpaOnnxOnlineModelConfig {
    SherpaOnnxOnlineTransducerModelConfig transducer;
    SherpaOnnxOnlineParaformerModelConfig paraformer;
    SherpaOnnxOnlineZipformer2CtcModelConfig zipformer2_ctc;
    const char *tokens;
    int32_t num_threads;
    const char *provider;
    int32_t debug;
    const char *model_type;
    const char *modeling_unit;
    const char *bpe_vocab;
    const char *tokens_buf;
    int32_t tokens_buf_size;
    SherpaOnnxOnlineNemoCtcModelConfig nemo_ctc;
    SherpaOnnxOnlineToneCtcModelConfig t_one_ctc;
};

struct SherpaOnnxFeatureConfig {
    int32_t sample_rate;
    int32_t feature_dim;
};

struct SherpaOnnxKeywordSpotterConfig {
    SherpaOnnxFeatureConfig feat_config;
    SherpaOnnxOnlineModelConfig model_config;
    int32_t max_active_paths;
    int32_t num_trailing_blanks;
    float keywords_score;
    float keywords_threshold;
    const char *keywords_file;
    const char *keywords_buf;
};

namespace voice {

// ---------------------------------------------------------------------------
// ctor / dtor
// ---------------------------------------------------------------------------

KwsEngine::KwsEngine() = default;

KwsEngine::~KwsEngine()
{
    deinit();
}

// ---------------------------------------------------------------------------
// loadLibrary / unloadLibrary
// ---------------------------------------------------------------------------

bool KwsEngine::loadLibrary()
{
    static const char *kLibNames[] = {
        "libsherpa-onnx-c-api.so",
        "libsherpa-onnx-c-api.so.1",
        nullptr
    };

    for (int i = 0; kLibNames[i] != nullptr; ++i) {
        m_handle = dlopen(kLibNames[i], RTLD_LAZY);
        if (m_handle) {
            VLOG_INFO(MODULE, "loaded %s", kLibNames[i]);
            return true;
        }
        const char *err = dlerror();
        VLOG_WARN(MODULE, "dlopen %s failed: %s",
                  kLibNames[i], err ? err : "unknown");
    }

    VLOG_ERROR(MODULE, "failed to load any sherpa-onnx library");
    return false;
}

void KwsEngine::unloadLibrary()
{
    // Destroy in reverse order of creation: stream -> spotter -> handle.
    if (m_stream) {
        if (m_fnDestroyStream) {
            m_fnDestroyStream(m_stream);
        }
        m_stream = nullptr;
    }

    if (m_spotter) {
        if (m_fnDestroySpotter) {
            m_fnDestroySpotter(m_spotter);
        }
        m_spotter = nullptr;
    }

    if (m_handle) {
        dlclose(m_handle);
        m_handle = nullptr;
    }
}

// ---------------------------------------------------------------------------
// loadSymbols: dlsym all KWS-specific functions + shared OnlineStream APIs
// ---------------------------------------------------------------------------

bool KwsEngine::loadSymbols()
{
#define LOAD_SYM(name, var) do { \
    var = reinterpret_cast<decltype(var)>(dlsym(m_handle, name)); \
    if (!var) { \
        VLOG_ERROR(MODULE, "symbol not found: %s (%s)", name, dlerror()); \
        return false; \
    } \
} while(0)

    // KWS-specific functions.
    LOAD_SYM("SherpaOnnxCreateKeywordSpotter", m_fnCreateSpotter);
    LOAD_SYM("SherpaOnnxDestroyKeywordSpotter", m_fnDestroySpotter);
    LOAD_SYM("SherpaOnnxCreateKeywordStream", m_fnCreateStream);
    LOAD_SYM("SherpaOnnxIsKeywordStreamReady", m_fnIsReady);
    LOAD_SYM("SherpaOnnxDecodeKeywordStream", m_fnDecode);
    LOAD_SYM("SherpaOnnxGetKeywordResult", m_fnGetResult);
    LOAD_SYM("SherpaOnnxDestroyKeywordResult", m_fnDestroyResult);
    LOAD_SYM("SherpaOnnxResetKeywordStream", m_fnReset);

    // Shared with OnlineRecognizer (KWS streams are SherpaOnnxOnlineStream*).
    LOAD_SYM("SherpaOnnxOnlineStreamAcceptWaveform", m_fnAcceptWaveform);
    LOAD_SYM("SherpaOnnxDestroyOnlineStream", m_fnDestroyStream);

#undef LOAD_SYM

    // Runtime version API (mandatory: ABI verification before struct use).
    m_fnGetVersionStr = reinterpret_cast<GetVersionStrFn>(
        dlsym(m_handle, "SherpaOnnxGetVersionStr"));
    m_fnGetGitSha1 = reinterpret_cast<GetGitSha1Fn>(
        dlsym(m_handle, "SherpaOnnxGetGitSha1"));
    m_fnGetGitDate = reinterpret_cast<GetGitDateFn>(
        dlsym(m_handle, "SherpaOnnxGetGitDate"));
    if (!m_fnGetVersionStr || !m_fnGetGitSha1 || !m_fnGetGitDate) {
        VLOG_ERROR(MODULE, "version query API missing; cannot validate ABI");
        return false;
    }

    VLOG_INFO(MODULE, "all required KWS symbols loaded");
    return true;
}

// ---------------------------------------------------------------------------
// loadRuntimeVersion: verify runtime matches 1.13.3 / 330609da
// ---------------------------------------------------------------------------

bool KwsEngine::loadRuntimeVersion()
{
    if (!m_fnGetVersionStr || !m_fnGetGitSha1 || !m_fnGetGitDate) {
        VLOG_ERROR(MODULE, "version API missing; cannot validate ABI");
        return false;
    }

    dlerror();
    const char *ver = m_fnGetVersionStr();
    const char *verErr = dlerror();
    if (verErr) {
        VLOG_ERROR(MODULE, "SherpaOnnxGetVersionStr failed: %s", verErr);
        return false;
    }

    dlerror();
    const char *sha = m_fnGetGitSha1();
    const char *shaErr = dlerror();
    if (shaErr) {
        VLOG_ERROR(MODULE, "SherpaOnnxGetGitSha1 failed: %s", shaErr);
        return false;
    }

    dlerror();
    const char *date = m_fnGetGitDate();
    const char *dateErr = dlerror();
    if (dateErr) {
        VLOG_ERROR(MODULE, "SherpaOnnxGetGitDate failed: %s", dateErr);
        return false;
    }

    // Copy to std::string to avoid holding raw lib pointers.
    m_runtimeVersion = ver ? ver : "";
    m_runtimeGitSha = sha ? sha : "";
    m_runtimeGitDate = date ? date : "";

    m_resultAbiCompatible = sherpa_abi_1_13_3::isCompatible(
        m_runtimeVersion.c_str(), m_runtimeGitSha.c_str());

    VLOG_INFO(MODULE, "SHERPA_VERSION=%s", m_runtimeVersion.c_str());
    VLOG_INFO(MODULE, "SHERPA_GIT_SHA=%s", m_runtimeGitSha.c_str());
    VLOG_INFO(MODULE, "SHERPA_GIT_DATE=%s", m_runtimeGitDate.c_str());
    VLOG_INFO(MODULE, "RESULT_ABI_COMPATIBLE=%d",
              m_resultAbiCompatible ? 1 : 0);

    if (!m_resultAbiCompatible) {
        VLOG_ERROR(MODULE,
            "unsupported sherpa KWS ABI: "
            "expected version=1.13.3 sha=330609da, "
            "actual version=%s sha=%s",
            m_runtimeVersion.c_str(), m_runtimeGitSha.c_str());
    }

    return true;
}

// ---------------------------------------------------------------------------
// init / deinit
// ---------------------------------------------------------------------------

bool KwsEngine::init(const KwsConfig &config)
{
    if (m_ready) {
        VLOG_WARN(MODULE, "already initialized, deinit first");
        return true;
    }

    m_config = config;

    if (!m_config.enabled) {
        VLOG_INFO(MODULE, "KWS disabled in config");
        return false;
    }

    // Validate required file paths before dlopen.
    if (m_config.encoderPath.empty() ||
        m_config.decoderPath.empty() ||
        m_config.joinerPath.empty() ||
        m_config.tokensPath.empty() ||
        m_config.keywordsFile.empty()) {
        VLOG_ERROR(MODULE,
            "init: one or more required paths are empty "
            "(encoder=%s, decoder=%s, joiner=%s, tokens=%s, keywords=%s)",
            m_config.encoderPath.c_str(),
            m_config.decoderPath.c_str(),
            m_config.joinerPath.c_str(),
            m_config.tokensPath.c_str(),
            m_config.keywordsFile.c_str());
        return false;
    }

    // Validate keywords file exists.
    {
        std::ifstream kf(m_config.keywordsFile);
        if (!kf.is_open()) {
            VLOG_ERROR(MODULE, "init: keywords_file not found: %s",
                       m_config.keywordsFile.c_str());
            return false;
        }
    }

    // Clamp tuning parameters to safe ranges.
    if (!std::isfinite(m_config.keywordsScore) || m_config.keywordsScore <= 0.0f) {
        VLOG_WARN(MODULE,
            "invalid keywords_score=%.2f, fall back to 1.0",
            static_cast<double>(m_config.keywordsScore));
        m_config.keywordsScore = 1.0f;
    }
    if (!std::isfinite(m_config.keywordsThreshold) ||
        m_config.keywordsThreshold < 0.0f ||
        m_config.keywordsThreshold > 1.0f) {
        VLOG_WARN(MODULE,
            "invalid keywords_threshold=%.2f, fall back to 0.25",
            static_cast<double>(m_config.keywordsThreshold));
        m_config.keywordsThreshold = 0.25f;
    }

    VLOG_INFO(MODULE, "KWS_KEYWORDS_FILE=%s", m_config.keywordsFile.c_str());
    VLOG_INFO(MODULE, "KWS_KEYWORDS_SCORE=%.2f",
              static_cast<double>(m_config.keywordsScore));
    VLOG_INFO(MODULE, "KWS_KEYWORDS_THRESHOLD=%.2f",
              static_cast<double>(m_config.keywordsThreshold));

    if (!loadLibrary()) {
        return false;
    }

    if (!loadSymbols()) {
        unloadLibrary();
        return false;
    }

    // ABI verification: refuse to pass our local struct if the runtime
    // does not match the verified 1.13.3 / 330609da layout.
    if (!loadRuntimeVersion() || !m_resultAbiCompatible) {
        VLOG_ERROR(MODULE,
                   "refusing to create keyword spotter with an unverified ABI");
        deinit();
        return false;
    }

    // Construct KeywordSpotterConfig (zero-initialized for forward compat).
    SherpaOnnxKeywordSpotterConfig spotterConfig;
    std::memset(&spotterConfig, 0, sizeof(spotterConfig));

    // Model configuration (transducer streaming model).
    spotterConfig.model_config.transducer.encoder = m_config.encoderPath.c_str();
    spotterConfig.model_config.transducer.decoder = m_config.decoderPath.c_str();
    spotterConfig.model_config.transducer.joiner = m_config.joinerPath.c_str();
    spotterConfig.model_config.tokens = m_config.tokensPath.c_str();
    spotterConfig.model_config.num_threads = m_config.numThreads;
    spotterConfig.model_config.provider = "cpu";
    spotterConfig.model_config.debug = 0;

    // Feature configuration.
    spotterConfig.feat_config.sample_rate = m_config.sampleRate;
    spotterConfig.feat_config.feature_dim = 80;

    // KWS tuning.
    spotterConfig.max_active_paths = m_config.maxActivePaths;
    spotterConfig.num_trailing_blanks = m_config.numTrailingBlanks;
    spotterConfig.keywords_score = m_config.keywordsScore;
    spotterConfig.keywords_threshold = m_config.keywordsThreshold;
    spotterConfig.keywords_file = m_config.keywordsFile.c_str();

    // Create keyword spotter (pass as void* to avoid ABI dependency).
    m_spotter = m_fnCreateSpotter(&spotterConfig);
    if (!m_spotter) {
        VLOG_ERROR(MODULE, "failed to create keyword spotter");
        unloadLibrary();
        return false;
    }

    // Create keyword stream (uses default keywords from keywords_file).
    m_stream = m_fnCreateStream(m_spotter);
    if (!m_stream) {
        VLOG_ERROR(MODULE, "failed to create keyword stream");
        m_fnDestroySpotter(m_spotter);
        m_spotter = nullptr;
        unloadLibrary();
        return false;
    }

    m_ready = true;
    VLOG_INFO(MODULE,
              "KWS engine initialized (model=%s, keywords=%s, sample_rate=%d, threads=%d)",
              m_config.modelPath.c_str(),
              m_config.keywordsFile.c_str(),
              m_config.sampleRate, m_config.numThreads);
    return true;
}

void KwsEngine::deinit()
{
    const bool hadResources =
        m_ready || m_handle != nullptr ||
        m_spotter != nullptr || m_stream != nullptr;
    unloadLibrary();
    m_ready = false;
    if (hadResources) {
        VLOG_INFO(MODULE, "KWS engine deinitialized");
    }
}

bool KwsEngine::isReady() const
{
    return m_ready;
}

// ---------------------------------------------------------------------------
// acceptWaveform / isReadyToDecode / decode
// ---------------------------------------------------------------------------

void KwsEngine::acceptWaveform(const float *samples, int32_t numSamples)
{
    if (!m_ready || !m_stream || !m_fnAcceptWaveform) {
        return;
    }
    m_fnAcceptWaveform(m_stream, m_config.sampleRate, samples, numSamples);
}

bool KwsEngine::isReadyToDecode() const
{
    if (!m_ready || !m_spotter || !m_stream || !m_fnIsReady) {
        return false;
    }
    return m_fnIsReady(m_spotter, m_stream) == 1;
}

void KwsEngine::decode()
{
    if (!m_ready || !m_spotter || !m_stream || !m_fnDecode) {
        return;
    }
    m_fnDecode(m_spotter, m_stream);
}

// ---------------------------------------------------------------------------
// getResult: read matched keyword (must be released via DestroyResult)
// ---------------------------------------------------------------------------

std::string KwsEngine::getResult() const
{
    if (!m_ready || !m_spotter || !m_stream ||
        !m_fnGetResult || !m_fnDestroyResult || !m_resultAbiCompatible) {
        return {};
    }

    const auto *result = m_fnGetResult(m_spotter, m_stream);
    if (!result) {
        return {};
    }

    std::string keyword;
    if (result->keyword && result->keyword[0] != '\0') {
        keyword = result->keyword;
    }

    // Always release the result pointer (matches AsrEngine pattern).
    m_fnDestroyResult(result);
    return keyword;
}

// ---------------------------------------------------------------------------
// reset: flush stream state after a keyword hit
//
// SherpaOnnxResetKeywordStream clears internal decoder state so the spotter
// can detect the next wake word. Without this, the same keyword fires again
// on the next decode cycle.
// ---------------------------------------------------------------------------

void KwsEngine::reset()
{
    if (!m_ready || !m_spotter || !m_stream || !m_fnReset) {
        return;
    }
    m_fnReset(m_spotter, m_stream);
}

// ---------------------------------------------------------------------------
// loadConfig: parse key=value file
// ---------------------------------------------------------------------------

KwsConfig KwsEngine::loadConfig(const std::string &path)
{
    KwsConfig config;

    std::ifstream file(path);
    if (!file.is_open()) {
        VLOG_WARN(MODULE, "cannot open KWS config: %s", path.c_str());
        return config;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);

        auto trim = [](std::string &s) {
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t") + 1);
        };
        trim(key);
        trim(val);

        if (key == "kws_enabled") {
            config.enabled = (val == "true" || val == "1");
        } else if (key == "kws_model_path") {
            config.modelPath = val;
        } else if (key == "kws_tokens_path") {
            config.tokensPath = val;
        } else if (key == "kws_encoder_path") {
            config.encoderPath = val;
        } else if (key == "kws_decoder_path") {
            config.decoderPath = val;
        } else if (key == "kws_joiner_path") {
            config.joinerPath = val;
        } else if (key == "kws_keywords_file") {
            config.keywordsFile = val;
        } else if (key == "kws_sample_rate") {
            try {
                config.sampleRate = std::stoi(val);
            } catch (const std::exception &) {
                VLOG_WARN(MODULE, "invalid kws_sample_rate: %s", val.c_str());
            }
        } else if (key == "kws_num_threads") {
            try {
                config.numThreads = std::stoi(val);
            } catch (const std::exception &) {
                VLOG_WARN(MODULE, "invalid kws_num_threads: %s", val.c_str());
            }
        } else if (key == "kws_keywords_score") {
            try {
                float score = std::stof(val);
                if (std::isfinite(score) && score > 0.0f) {
                    config.keywordsScore = score;
                } else {
                    VLOG_WARN(MODULE,
                        "invalid kws_keywords_score: %s (must be >0 and finite; "
                        "fall back to 1.0)", val.c_str());
                }
            } catch (const std::exception &) {
                VLOG_WARN(MODULE,
                    "invalid kws_keywords_score: %s (fall back to 1.0)",
                    val.c_str());
            }
        } else if (key == "kws_keywords_threshold") {
            try {
                float threshold = std::stof(val);
                if (std::isfinite(threshold) &&
                    threshold >= 0.0f && threshold <= 1.0f) {
                    config.keywordsThreshold = threshold;
                } else {
                    VLOG_WARN(MODULE,
                        "invalid kws_keywords_threshold: %s (must be in [0,1]; "
                        "fall back to 0.25)", val.c_str());
                }
            } catch (const std::exception &) {
                VLOG_WARN(MODULE,
                    "invalid kws_keywords_threshold: %s (fall back to 0.25)",
                    val.c_str());
            }
        } else if (key == "kws_max_active_paths") {
            try {
                config.maxActivePaths = std::stoi(val);
            } catch (const std::exception &) {
                VLOG_WARN(MODULE, "invalid kws_max_active_paths: %s", val.c_str());
            }
        } else if (key == "kws_num_trailing_blanks") {
            try {
                config.numTrailingBlanks = std::stoi(val);
            } catch (const std::exception &) {
                VLOG_WARN(MODULE, "invalid kws_num_trailing_blanks: %s", val.c_str());
            }
        }
    }

    VLOG_INFO(MODULE,
              "loaded KWS config: enabled=%d, model=%s, keywords=%s",
              config.enabled ? 1 : 0,
              config.modelPath.c_str(),
              config.keywordsFile.c_str());
    return config;
}

} // namespace voice
