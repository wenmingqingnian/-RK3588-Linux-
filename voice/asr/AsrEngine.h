#ifndef ASR_ENGINE_H
#define ASR_ENGINE_H

/**
 * @file AsrEngine.h
 * @brief Offline ASR engine wrapper using sherpa-onnx C API.
 *
 * Uses dlopen to load sherpa-onnx shared libraries at runtime,
 * following the same pattern as rga_converter and rknn_detector.
 * No compile-time dependency on sherpa-onnx headers.
 *
 * Data flow:
 *   Microphone (ALSA) -> PCM float -> SherpaOnnxOnlineStream -> Recognized text
 */

#include <string>
#include <cstdint>

#include "sherpa_onnx_abi_1_13_3.h"

namespace voice {

struct AsrConfig {
    bool enabled = false;
    std::string modelPath;       // Path to sherpa-onnx model directory
    std::string tokensPath;      // Path to tokens.txt
    std::string encoderPath;     // Path to encoder model (streaming)
    std::string decoderPath;     // Path to decoder model
    std::string joinerPath;      // Path to joiner model
    int sampleRate = 16000;
    int numThreads = 2;
    std::string alsaDevice;      // ALSA device name (e.g., "plughw:3,0")

    // 热词解码配置（配置驱动）：
    //   decoding_method 仅允许 "greedy_search" 或 "modified_beam_search"。
    //   hotwordsFile 为空表示不使用热词文件。
    //   hotwordsScore 必须 >0 且有限；非法值回退默认 1.5。
    //   modified_beam_search 必须搭配非空且存在的 hotwordsFile。
    std::string decodingMethod = "greedy_search";
    std::string hotwordsFile;
    float hotwordsScore = 1.5f;
};

class AsrEngine {
public:
    AsrEngine();
    ~AsrEngine();

    // Non-copyable, non-movable (owns dlopen handle and library resources)
    AsrEngine(const AsrEngine &) = delete;
    AsrEngine &operator=(const AsrEngine &) = delete;
    AsrEngine(AsrEngine &&) = delete;
    AsrEngine &operator=(AsrEngine &&) = delete;

    bool init(const AsrConfig &config);
    void deinit();
    bool isReady() const;

    /**
     * @brief Accept audio samples for recognition.
     * @param samples Float audio samples normalized to [-1, 1]
     * @param numSamples Number of samples
     */
    void acceptWaveform(const float *samples, int32_t numSamples);

    /**
     * @brief Check if there are enough frames for decoding.
     * @return true if ready to decode
     */
    bool isReadyToDecode() const;

    /**
     * @brief Run one decoding step.
     */
    void decode();

    /**
     * @brief Get the current recognition result.
     * @return Recognized text so far
     */
    std::string getResult() const;

    /**
     * @brief Check if an endpoint has been detected.
     * @return true if endpoint detected (user stopped speaking)
     */
    bool isEndpoint() const;

    /**
     * @brief Reset the stream for next utterance.
     */
    void reset();

    /**
     * @brief Signal that no more audio will be sent (file ended).
     *
     * Calls SherpaOnnxOnlineStreamInputFinished if available in the library.
     * If the symbol is not available, this is a no-op and the caller should
     * append trailing silence to trigger endpoint detection.
     *
     * @return true if the API was called, false if not available
     */
    bool inputFinished();

    /**
     * @brief Check if inputFinished() API is available.
     */
    bool hasInputFinishedApi() const;

    /**
     * @brief Load config from file.
     */
    static AsrConfig loadConfig(const std::string &path);

    /**
     * @brief 查询运行时 sherpa-onnx 版本字符串。
     * @return 版本字符串（如 "1.13.3"），库未加载或符号缺失时返回空。
     */
    const std::string &runtimeVersion() const { return m_runtimeVersion; }

    /**
     * @brief 查询运行时 sherpa-onnx Git SHA。
     * @return Git SHA 字符串（如 "330609da"），库未加载或符号缺失时返回空。
     */
    const std::string &runtimeGitSha() const { return m_runtimeGitSha; }

    /**
     * @brief 查询运行时 sherpa-onnx Git Date。
     */
    const std::string &runtimeGitDate() const { return m_runtimeGitDate; }

    /**
     * @brief 查询运行时库是否与已确认 ABI (1.13.3 / 330609da) 兼容。
     *
     * 仅当此值为 true 时，getResult() 才会按官方结构读取 result->text；
     * 否则 getResult() 返回空字符串，禁止猜测偏移。
     */
    bool isResultAbiCompatible() const { return m_resultAbiCompatible; }

private:
    bool loadLibrary();
    void unloadLibrary();
    bool loadSymbols();
    bool loadRuntimeVersion();

    // Opaque handles from sherpa-onnx library (void* to avoid header dependency)
    void *m_handle = nullptr;           // dlopen handle for libsherpa-onnx-c-api.so
    void *m_recognizer = nullptr;       // SherpaOnnxOnlineRecognizer*
    void *m_stream = nullptr;           // SherpaOnnxOnlineStream*
    AsrConfig m_config;
    bool m_ready = false;

    // sherpa-onnx C API function pointers (loaded via dlsym)
    // Using void* for opaque handles to avoid struct ABI mismatch
    using FnCreateRecognizer = void* (*)(const void *config);
    using FnDestroyRecognizer = void (*)(void *recognizer);
    using FnCreateStream = void* (*)(void *recognizer);
    using FnDestroyStream = void (*)(void *stream);
    using FnAcceptWaveform = void (*)(void *stream, int32_t sample_rate,
                                      const float *samples, int32_t n);
    using FnIsReady = int32_t (*)(const void *recognizer, const void *stream);
    using FnDecode = void (*)(void *recognizer, void *stream);
    using FnGetResult = const sherpa_abi_1_13_3::OnlineRecognizerResult* (*)(
        const void *recognizer, const void *stream);
    using FnDestroyResult = void (*)(
        const sherpa_abi_1_13_3::OnlineRecognizerResult *result);
    using FnIsEndpoint = int32_t (*)(const void *recognizer, const void *stream);
    using FnReset = void (*)(void *recognizer, void *stream);
    using FnInputFinished = void (*)(void *stream);

    // 运行时版本查询 API（用于在读取 result 前校验 ABI 兼容性）
    using GetVersionStrFn = const char *(*)();
    using GetGitSha1Fn = const char *(*)();
    using GetGitDateFn = const char *(*)();

    FnCreateRecognizer m_fnCreateRecognizer = nullptr;
    FnDestroyRecognizer m_fnDestroyRecognizer = nullptr;
    FnCreateStream m_fnCreateStream = nullptr;
    FnDestroyStream m_fnDestroyStream = nullptr;
    FnAcceptWaveform m_fnAcceptWaveform = nullptr;
    FnIsReady m_fnIsReady = nullptr;
    FnDecode m_fnDecode = nullptr;
    FnGetResult m_fnGetResult = nullptr;
    FnDestroyResult m_fnDestroyResult = nullptr;
    FnIsEndpoint m_fnIsEndpoint = nullptr;
    FnReset m_fnReset = nullptr;
    FnInputFinished m_fnInputFinished = nullptr;
    GetVersionStrFn m_fnGetVersionStr = nullptr;
    GetGitSha1Fn m_fnGetGitSha1 = nullptr;
    GetGitDateFn m_fnGetGitDate = nullptr;

    // 运行时版本信息（init 时从库内复制一份，避免长期持有库内部裸指针）
    std::string m_runtimeVersion;
    std::string m_runtimeGitSha;
    std::string m_runtimeGitDate;

    // 仅当版本与 Git SHA 同时匹配 1.13.3 / 330609da 时为 true
    bool m_resultAbiCompatible = false;
};

} // namespace voice

#endif // ASR_ENGINE_H
