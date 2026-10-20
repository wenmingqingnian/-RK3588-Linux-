#ifndef KWS_ENGINE_H
#define KWS_ENGINE_H

/**
 * @file KwsEngine.h
 * @brief Offline keyword spotter (KWS) wrapper using sherpa-onnx C API.
 *
 * Detects wake words from a continuous audio stream. Uses the same
 * dlopen + ABI verification pattern as AsrEngine. No compile-time
 * dependency on sherpa-onnx headers.
 *
 * Key differences from AsrEngine:
 *   - No endpoint detection (KWS runs continuously, never "stops speaking")
 *   - getResult() returns the matched keyword (e.g. "你好小问") or empty
 *   - After a hit, reset() MUST be called or the same keyword fires again
 *   - SherpaOnnxGetKeywordResult returns a pointer that the caller must
 *     release via SherpaOnnxDestroyKeywordResult
 *
 * Data flow:
 *   Microphone (ALSA) -> PCM float -> SherpaOnnxKeywordStream -> Keyword hit
 *
 * Lifecycle: init() -> acceptWaveform()* -> isReadyToDecode/decode/getResult
 *            -> reset() on hit -> ... -> deinit()
 */

#include <string>
#include <cstdint>

#include "sherpa_onnx_abi_1_13_3.h"

namespace voice {

struct KwsConfig {
    bool enabled = false;
    std::string modelPath;        // Path to sherpa-onnx KWS model directory
    std::string tokensPath;       // Path to tokens.txt
    std::string encoderPath;      // Path to encoder model (streaming transducer)
    std::string decoderPath;      // Path to decoder model
    std::string joinerPath;       // Path to joiner model
    std::string keywordsFile;     // Path to keywords.txt (token seq + @display)
    int sampleRate = 16000;
    int numThreads = 2;

    // KWS tuning (see sherpa-onnx KWS docs):
    //   keywordsScore: token bonus; increase to reduce misses, decrease
    //                  to reduce false alarms. Typical 1.0-2.0.
    //   keywordsThreshold: trigger threshold; increase to reduce false
    //                     alarms. Typical 0.1-0.5.
    //   maxActivePaths: modified_beam_search beam width (default 4).
    //   numTrailingBlanks: blank frames after keyword before firing.
    //                      Multi-syllable overlap: increase to ~8.
    float keywordsScore = 1.0f;
    float keywordsThreshold = 0.25f;
    int maxActivePaths = 4;
    int numTrailingBlanks = 1;
};

class KwsEngine {
public:
    KwsEngine();
    ~KwsEngine();

    // Non-copyable, non-movable (owns dlopen handle and library resources)
    KwsEngine(const KwsEngine &) = delete;
    KwsEngine &operator=(const KwsEngine &) = delete;
    KwsEngine(KwsEngine &&) = delete;
    KwsEngine &operator=(KwsEngine &&) = delete;

    bool init(const KwsConfig &config);
    void deinit();
    bool isReady() const;

    /**
     * @brief Accept audio samples for keyword spotting.
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
     * @brief Get the current keyword result.
     * @return Matched keyword display name (e.g. "你好小问") or empty.
     *
     * After a non-empty return, the caller MUST call reset() to clear the
     * stream state, otherwise the same keyword will fire again on the
     * next decode cycle.
     */
    std::string getResult() const;

    /**
     * @brief Reset the stream after a keyword hit.
     *
     * Calls SherpaOnnxResetKeywordStream to flush internal state so the
     * spotter can detect the next wake word. Safe to call when no hit
     * has occurred (clears any partial state).
     */
    void reset();

    /**
     * @brief Load KWS config from a key=value file.
     */
    static KwsConfig loadConfig(const std::string &path);

    // --- Runtime version introspection (same pattern as AsrEngine) ---
    const std::string &runtimeVersion() const { return m_runtimeVersion; }
    const std::string &runtimeGitSha() const { return m_runtimeGitSha; }
    const std::string &runtimeGitDate() const { return m_runtimeGitDate; }

    /**
     * @brief Query whether the runtime ABI matches 1.13.3 / 330609da.
     *
     * Only when this is true will getResult() read result->keyword;
     * otherwise getResult() returns empty (refuses to guess offsets).
     */
    bool isResultAbiCompatible() const { return m_resultAbiCompatible; }

private:
    bool loadLibrary();
    void unloadLibrary();
    bool loadSymbols();
    bool loadRuntimeVersion();

    // Opaque handles (void* to avoid header dependency on c-api.h)
    void *m_handle = nullptr;          // dlopen handle
    void *m_spotter = nullptr;          // SherpaOnnxKeywordSpotter*
    void *m_stream = nullptr;           // SherpaOnnxOnlineStream* (KWS uses online streams too)
    KwsConfig m_config;
    bool m_ready = false;

    // --- Function pointer types ---
    // Note: KWS stream type is SherpaOnnxOnlineStream (same as ASR),
    //       so acceptWaveform / destroyStream reuse the OnlineStream API.
    using FnCreateSpotter = void* (*)(const void *config);
    using FnDestroySpotter = void (*)(void *spotter);
    using FnCreateStream = void* (*)(void *spotter);
    using FnDestroyStream = void (*)(void *stream);
    using FnAcceptWaveform = void (*)(void *stream, int32_t sample_rate,
                                      const float *samples, int32_t n);
    using FnIsReady = int32_t (*)(const void *spotter, const void *stream);
    using FnDecode = void (*)(void *spotter, void *stream);
    using FnGetResult = const sherpa_abi_1_13_3::KeywordResult* (*)(
        const void *spotter, const void *stream);
    using FnDestroyResult = void (*)(
        const sherpa_abi_1_13_3::KeywordResult *result);
    using FnReset = void (*)(void *spotter, void *stream);

    // Runtime version query (shared with AsrEngine)
    using GetVersionStrFn = const char *(*)();
    using GetGitSha1Fn = const char *(*)();
    using GetGitDateFn = const char *(*)();

    FnCreateSpotter   m_fnCreateSpotter = nullptr;
    FnDestroySpotter  m_fnDestroySpotter = nullptr;
    FnCreateStream    m_fnCreateStream = nullptr;
    FnDestroyStream   m_fnDestroyStream = nullptr;
    FnAcceptWaveform  m_fnAcceptWaveform = nullptr;
    FnIsReady         m_fnIsReady = nullptr;
    FnDecode          m_fnDecode = nullptr;
    FnGetResult       m_fnGetResult = nullptr;
    FnDestroyResult   m_fnDestroyResult = nullptr;
    FnReset           m_fnReset = nullptr;
    GetVersionStrFn   m_fnGetVersionStr = nullptr;
    GetGitSha1Fn      m_fnGetGitSha1 = nullptr;
    GetGitDateFn      m_fnGetGitDate = nullptr;

    // Runtime version info (copied once at init to avoid holding raw lib pointers)
    std::string m_runtimeVersion;
    std::string m_runtimeGitSha;
    std::string m_runtimeGitDate;

    // Only true when version==1.13.3 AND gitSha==330609da
    bool m_resultAbiCompatible = false;
};

} // namespace voice

#endif // KWS_ENGINE_H
