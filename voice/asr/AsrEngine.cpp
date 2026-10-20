/**
 * @file AsrEngine.cpp
 * @brief Offline ASR engine implementation using sherpa-onnx via dlopen.
 *
 * Follows the same dlopen/dlsym pattern as rga_converter.cpp and rknn_detector.cpp.
 * No compile-time dependency on sherpa-onnx headers.
 *
 * ABI safety: all sherpa-onnx handles are void*, struct definitions are local
 * and only used for config construction. The actual struct layout is determined
 * by the installed library version at runtime.
 *
 * Migrated from LLM_Voice_Flow-master/voice/sherpa-onnx/c-api-examples/c-api-alsa.cc
 */

#include "AsrEngine.h"
#include "logmanager.h"
#include "sherpa_onnx_abi_1_13_3.h"

#include <dlfcn.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

static const char *MODULE = "AsrEngine";

// sherpa-onnx C API struct definitions for config construction only.
// These must match the layout expected by the installed sherpa-onnx library.
// If the library version changes, these definitions must be updated.
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

struct SherpaOnnxOnlineCtcFstDecoderConfig {
    const char *graph;
    int32_t max_active;
};

struct SherpaOnnxHomophoneReplacerConfig {
    const char *dict_dir;
    const char *lexicon;
    const char *rule_fsts;
};

struct SherpaOnnxOnlineRecognizerConfig {
    SherpaOnnxFeatureConfig feat_config;
    SherpaOnnxOnlineModelConfig model_config;
    const char *decoding_method;
    int32_t max_active_paths;
    int32_t enable_endpoint;
    float rule1_min_trailing_silence;
    float rule2_min_trailing_silence;
    float rule3_min_utterance_length;
    const char *hotwords_file;
    float hotwords_score;
    SherpaOnnxOnlineCtcFstDecoderConfig ctc_fst_decoder_config;
    const char *rule_fsts;
    const char *rule_fars;
    float blank_penalty;
    const char *hotwords_buf;
    int32_t hotwords_buf_size;
    SherpaOnnxHomophoneReplacerConfig hr;
};

namespace voice {

AsrEngine::AsrEngine() = default;

AsrEngine::~AsrEngine()
{
    deinit();
}

bool AsrEngine::loadLibrary()
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
        VLOG_WARN(MODULE, "dlopen %s failed: %s", kLibNames[i], err ? err : "unknown");
    }

    VLOG_ERROR(MODULE, "failed to load any sherpa-onnx library");
    return false;
}

void AsrEngine::unloadLibrary()
{
    if (m_stream) {
        if (m_fnDestroyStream) {
            m_fnDestroyStream(m_stream);
        }
        m_stream = nullptr;
    }

    if (m_recognizer) {
        if (m_fnDestroyRecognizer) {
            m_fnDestroyRecognizer(m_recognizer);
        }
        m_recognizer = nullptr;
    }

    if (m_handle) {
        dlclose(m_handle);
        m_handle = nullptr;
    }
}

bool AsrEngine::loadSymbols()
{
    // Macro to load a symbol and report failure
#define LOAD_SYM(name, var) do { \
    var = reinterpret_cast<decltype(var)>(dlsym(m_handle, name)); \
    if (!var) { \
        VLOG_ERROR(MODULE, "symbol not found: %s (%s)", name, dlerror()); \
        return false; \
    } \
} while(0)

    LOAD_SYM("SherpaOnnxCreateOnlineRecognizer", m_fnCreateRecognizer);
    LOAD_SYM("SherpaOnnxDestroyOnlineRecognizer", m_fnDestroyRecognizer);
    LOAD_SYM("SherpaOnnxCreateOnlineStream", m_fnCreateStream);
    LOAD_SYM("SherpaOnnxDestroyOnlineStream", m_fnDestroyStream);
    LOAD_SYM("SherpaOnnxOnlineStreamAcceptWaveform", m_fnAcceptWaveform);
    LOAD_SYM("SherpaOnnxIsOnlineStreamReady", m_fnIsReady);
    LOAD_SYM("SherpaOnnxDecodeOnlineStream", m_fnDecode);
    LOAD_SYM("SherpaOnnxGetOnlineStreamResult", m_fnGetResult);
    LOAD_SYM("SherpaOnnxDestroyOnlineRecognizerResult", m_fnDestroyResult);
    LOAD_SYM("SherpaOnnxOnlineStreamIsEndpoint", m_fnIsEndpoint);
    LOAD_SYM("SherpaOnnxOnlineStreamReset", m_fnReset);

    // 版本/SHA/Date API 是安全读取 result 结构的必需前提，缺失则禁止访问结构体。
    LOAD_SYM("SherpaOnnxGetVersionStr", m_fnGetVersionStr);
    LOAD_SYM("SherpaOnnxGetGitSha1", m_fnGetGitSha1);
    LOAD_SYM("SherpaOnnxGetGitDate", m_fnGetGitDate);

    // InputFinished is optional; if not available, caller must append silence
    m_fnInputFinished = reinterpret_cast<FnInputFinished>(
        dlsym(m_handle, "SherpaOnnxOnlineStreamInputFinished"));
    if (m_fnInputFinished) {
        VLOG_INFO(MODULE, "SherpaOnnxOnlineStreamInputFinished found");
    } else {
        VLOG_WARN(MODULE, "SherpaOnnxOnlineStreamInputFinished not found; "
                          "caller must append trailing silence");
    }

#undef LOAD_SYM

    VLOG_INFO(MODULE, "all required symbols loaded");
    return true;
}

bool AsrEngine::loadRuntimeVersion()
{
    if (!m_fnGetVersionStr || !m_fnGetGitSha1 || !m_fnGetGitDate) {
        VLOG_ERROR(MODULE, "version API missing; cannot validate result ABI");
        return false;
    }

    // 调用前先清空 dlerror，避免误读旧错误
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

    // 立即复制到 std::string，避免长期持有库内部裸指针
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
            "unsupported sherpa result ABI: "
            "expected version=1.13.3 sha=330609da, "
            "actual version=%s sha=%s",
            m_runtimeVersion.c_str(), m_runtimeGitSha.c_str());
    }

    return true;
}

bool AsrEngine::init(const AsrConfig &config)
{
    if (m_ready) {
        VLOG_WARN(MODULE, "already initialized, deinit first");
        return true;
    }

    m_config = config;

    if (!m_config.enabled) {
        VLOG_INFO(MODULE, "ASR disabled in config");
        return false;
    }

    // 组合校验：decoding_method 与 hotwords_file 必须搭配正确。
    //   - greedy_search + 空 hotwords: 允许
    //   - modified_beam_search + 有效 hotwords: 允许
    //   - modified_beam_search + 空 hotwords: 初始化失败
    //   - hotwords 非空但文件不存在: 初始化失败
    //   - greedy_search + 非空 hotwords: 拒绝（禁止静默忽略）
    // 注意：loadConfig() 已经过滤非法 decoding_method，此处只处理组合关系。
    if (m_config.decodingMethod == "modified_beam_search") {
        if (m_config.hotwordsFile.empty()) {
            VLOG_ERROR(MODULE,
                "init: modified_beam_search requires non-empty hotwords_file "
                "(decoding_method=%s, hotwords_file=<empty>)",
                m_config.decodingMethod.c_str());
            return false;
        }
        std::ifstream hf(m_config.hotwordsFile);
        if (!hf.is_open()) {
            VLOG_ERROR(MODULE,
                "init: hotwords_file not found: %s (decoding_method=%s)",
                m_config.hotwordsFile.c_str(),
                m_config.decodingMethod.c_str());
            return false;
        }
    } else if (m_config.decodingMethod == "greedy_search") {
        if (!m_config.hotwordsFile.empty()) {
            VLOG_ERROR(MODULE,
                "init: greedy_search must not be combined with hotwords_file "
                "(decoding_method=%s, hotwords_file=%s); refusing to silently ignore",
                m_config.decodingMethod.c_str(),
                m_config.hotwordsFile.c_str());
            return false;
        }
    }

    // 运行时配置日志（仅输出热词解码相关字段，不含无关数据）
    VLOG_INFO(MODULE, "ASR_DECODING_METHOD=%s", m_config.decodingMethod.c_str());
    VLOG_INFO(MODULE, "ASR_HOTWORDS_FILE=%s",
              m_config.hotwordsFile.empty() ? "<empty>" : m_config.hotwordsFile.c_str());
    VLOG_INFO(MODULE, "ASR_HOTWORDS_SCORE=%.2f",
              static_cast<double>(m_config.hotwordsScore));

    if (!loadLibrary()) {
        return false;
    }

    if (!loadSymbols()) {
        unloadLibrary();
        return false;
    }

    // 在传递本地 ABI 结构体前校验固定的运行时版本和 Git SHA。
    // 不兼容时拒绝初始化，禁止把未知布局传给运行时。
    if (!loadRuntimeVersion() || !m_resultAbiCompatible) {
        VLOG_ERROR(MODULE,
                   "refusing to create recognizer with an unverified Sherpa ABI");
        deinit();
        return false;
    }

    // Configure online recognizer (zero-initialized for forward compatibility)
    SherpaOnnxOnlineRecognizerConfig recognizerConfig;
    memset(&recognizerConfig, 0, sizeof(recognizerConfig));

    // Model configuration
    recognizerConfig.model_config.transducer.encoder = m_config.encoderPath.c_str();
    recognizerConfig.model_config.transducer.decoder = m_config.decoderPath.c_str();
    recognizerConfig.model_config.transducer.joiner = m_config.joinerPath.c_str();
    recognizerConfig.model_config.tokens = m_config.tokensPath.c_str();
    recognizerConfig.model_config.num_threads = m_config.numThreads;
    recognizerConfig.model_config.provider = "cpu";
    recognizerConfig.model_config.debug = 0;

    // Feature configuration
    recognizerConfig.feat_config.sample_rate = m_config.sampleRate;
    recognizerConfig.feat_config.feature_dim = 80;

    // Decoding configuration (config-driven hotword biasing)
    recognizerConfig.decoding_method = m_config.decodingMethod.c_str();
    recognizerConfig.hotwords_file = m_config.hotwordsFile.empty()
                                         ? nullptr
                                         : m_config.hotwordsFile.c_str();
    recognizerConfig.hotwords_score = m_config.hotwordsScore;
    recognizerConfig.max_active_paths = 4;

    // Endpoint detection
    recognizerConfig.enable_endpoint = 1;
    recognizerConfig.rule1_min_trailing_silence = 2.4f;
    recognizerConfig.rule2_min_trailing_silence = 1.2f;
    recognizerConfig.rule3_min_utterance_length = 300.0f;

    // Create recognizer (pass as void* to avoid ABI dependency)
    m_recognizer = m_fnCreateRecognizer(&recognizerConfig);
    if (!m_recognizer) {
        VLOG_ERROR(MODULE, "failed to create online recognizer");
        unloadLibrary();
        return false;
    }

    // Create stream
    m_stream = m_fnCreateStream(m_recognizer);
    if (!m_stream) {
        VLOG_ERROR(MODULE, "failed to create online stream");
        m_fnDestroyRecognizer(m_recognizer);
        m_recognizer = nullptr;
        unloadLibrary();
        return false;
    }

    m_ready = true;
    VLOG_INFO(MODULE, "ASR engine initialized (model=%s, sample_rate=%d, threads=%d)",
              m_config.modelPath.c_str(), m_config.sampleRate, m_config.numThreads);
    return true;
}

void AsrEngine::deinit()
{
    const bool hadResources =
        m_ready || m_handle != nullptr || m_recognizer != nullptr || m_stream != nullptr;
    unloadLibrary();
    m_ready = false;
    if (hadResources) {
        VLOG_INFO(MODULE, "ASR engine deinitialized");
    }
}

bool AsrEngine::isReady() const
{
    return m_ready;
}

void AsrEngine::acceptWaveform(const float *samples, int32_t numSamples)
{
    if (!m_ready || !m_stream || !m_fnAcceptWaveform) {
        return;
    }

    m_fnAcceptWaveform(m_stream, m_config.sampleRate, samples, numSamples);
}

bool AsrEngine::isReadyToDecode() const
{
    if (!m_ready || !m_recognizer || !m_stream || !m_fnIsReady) {
        return false;
    }

    return m_fnIsReady(m_recognizer, m_stream) == 1;
}

void AsrEngine::decode()
{
    if (!m_ready || !m_recognizer || !m_stream || !m_fnDecode) {
        return;
    }

    m_fnDecode(m_recognizer, m_stream);
}

std::string AsrEngine::getResult() const
{
    if (!m_ready || !m_recognizer || !m_stream || !m_fnGetResult || !m_fnDestroyResult) {
        return {};
    }

    const auto *result = m_fnGetResult(m_recognizer, m_stream);
    if (!result) {
        return {};
    }

    // RAII guard：保证 rawResult 在所有路径下恰好 destroy 一次，
    // 包括 ABI 不兼容、text 为空、std::string 抛异常等情形。
    struct ResultGuard {
        FnDestroyResult destroyFn;
        const sherpa_abi_1_13_3::OnlineRecognizerResult *ptr;
        ~ResultGuard() {
            if (destroyFn && ptr) {
                destroyFn(ptr);
            }
        }
    } guard{m_fnDestroyResult, result};

    // ABI 不兼容时禁止读取结构体字段，但仍需 destroy rawResult。
    if (!m_resultAbiCompatible) {
        VLOG_ERROR(MODULE, "getResult: result ABI not compatible; "
                          "refusing to read struct fields");
        return {};
    }

    std::string text;
    if (result->text) {
        text.assign(result->text);
    }

    VLOG_INFO(MODULE, "RESULT_API=typed-abi-1.13.3");
    // guard 析构时 destroy rawResult，此后不再访问 result 任何字段
    return text;
}

bool AsrEngine::isEndpoint() const
{
    if (!m_ready || !m_recognizer || !m_stream || !m_fnIsEndpoint) {
        return false;
    }

    return m_fnIsEndpoint(m_recognizer, m_stream) == 1;
}

void AsrEngine::reset()
{
    if (!m_ready || !m_recognizer || !m_stream || !m_fnReset) {
        return;
    }

    m_fnReset(m_recognizer, m_stream);
}

bool AsrEngine::inputFinished()
{
    if (!m_ready || !m_stream || !m_fnInputFinished) {
        return false;
    }

    m_fnInputFinished(m_stream);
    return true;
}

bool AsrEngine::hasInputFinishedApi() const
{
    return m_fnInputFinished != nullptr;
}

AsrConfig AsrEngine::loadConfig(const std::string &path)
{
    AsrConfig config;

    std::ifstream file(path);
    if (!file.is_open()) {
        VLOG_WARN(MODULE, "cannot open ASR config: %s", path.c_str());
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

        // Trim whitespace
        auto trim = [](std::string &s) {
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t") + 1);
        };
        trim(key);
        trim(val);

        if (key == "asr_enabled") {
            config.enabled = (val == "true" || val == "1");
        } else if (key == "asr_model_path") {
            config.modelPath = val;
        } else if (key == "asr_tokens_path") {
            config.tokensPath = val;
        } else if (key == "asr_encoder_path") {
            config.encoderPath = val;
        } else if (key == "asr_decoder_path") {
            config.decoderPath = val;
        } else if (key == "asr_joiner_path") {
            config.joinerPath = val;
        } else if (key == "asr_sample_rate") {
            try {
                config.sampleRate = std::stoi(val);
            } catch (const std::exception &e) {
                VLOG_WARN(MODULE, "invalid asr_sample_rate: %s", val.c_str());
            }
        } else if (key == "asr_num_threads") {
            try {
                config.numThreads = std::stoi(val);
            } catch (const std::exception &e) {
                VLOG_WARN(MODULE, "invalid asr_num_threads: %s", val.c_str());
            }
        } else if (key == "asr_alsa_device") {
            config.alsaDevice = val;
        } else if (key == "asr_decoding_method") {
            // 仅允许 greedy_search / modified_beam_search；非法值回退默认。
            if (val == "greedy_search" || val == "modified_beam_search") {
                config.decodingMethod = val;
            } else {
                VLOG_WARN(MODULE,
                    "invalid asr_decoding_method: %s (fall back to greedy_search)",
                    val.c_str());
                // 保持默认 "greedy_search"
            }
        } else if (key == "asr_hotwords_file") {
            config.hotwordsFile = val;
        } else if (key == "asr_hotwords_score") {
            // 仅接受有限且 >0 的值；非法值回退默认 1.5。
            try {
                float score = std::stof(val);
                if (std::isfinite(score) && score > 0.0f) {
                    config.hotwordsScore = score;
                } else {
                    VLOG_WARN(MODULE,
                        "invalid asr_hotwords_score: %s (must be >0 and finite; "
                        "fall back to 1.5)", val.c_str());
                    // 保持默认 1.5
                }
            } catch (const std::exception &e) {
                VLOG_WARN(MODULE,
                    "invalid asr_hotwords_score: %s (fall back to 1.5)",
                    val.c_str());
                // 保持默认 1.5
            }
        }
    }

    VLOG_INFO(MODULE, "loaded ASR config: enabled=%d, model=%s, device=%s",
              config.enabled ? 1 : 0, config.modelPath.c_str(),
              config.alsaDevice.c_str());
    return config;
}

} // namespace voice
