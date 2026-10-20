#ifndef VOICE_CONFIG_H
#define VOICE_CONFIG_H

/**
 * @file voice_config.h
 * @brief Voice pipeline configuration value type and loader.
 *
 * Pure C++ (no Qt dependency) so the parsing logic can be unit-tested with
 * a plain C++ compiler. VoiceManager includes this header and delegates
 * loadConfig() to voice::loadVoiceConfig().
 */

#include <string>

namespace voice {

/**
 * @brief Voice pipeline configuration.
 *
 * Safety defaults (section 二 of the implementation spec):
 *   - llmEnabled / ttsEnabled default to false (never auto-start LLM/TTS,
 *     never auto-access the NPU driver).
 *   - directCommandEnabled defaults to true so the ASR -> fixed-command ->
 *     GUI path works out of the box.
 *   - unknownCommandFallbackToLlm defaults to true so that, when LLM is
 *     explicitly enabled later, unmatched commands naturally fall back to
 *     the LLM. When LLM is disabled (the current board state) this flag has
 *     no effect and unmatched commands are simply logged.
 */
struct VoiceConfig {
    bool enabled = false;
    bool llmEnabled = false;
    bool ttsEnabled = false;
    bool directCommandEnabled = true;
    bool unknownCommandFallbackToLlm = true;

    // TTS feedback for fixed commands. When true, a matched cabin command
    // triggers a short TTS announcement (e.g. "已为您打开地图"). This works
    // without LLM: VoiceManager sends the feedback text directly to the
    // TTS service via ZMQ. Default false to preserve current behavior.
    bool ttsFeedbackForCommand = false;

    // ASR mute window during TTS feedback playback (milliseconds).
    // When sendToTts() is called, ASR capture is muted for this duration
    // AFTER the TTS ack is received, to prevent the microphone from picking
    // up the TTS playback and creating an ASR->TTS->ASR echo loop.
    // The ack returns before playback completes (TTS synthesis+playback is
    // async on the service side), so this window covers the actual playback.
    // Short cabin feedback phrases (~6 chars) synthesize+play in ~1-2s on
    // RK3588S; 3000ms is a safe default. Set higher if TTS phrases are long.
    int ttsFeedbackMuteMs = 3000;

    std::string llmBinPath;        // Path to LLM executable
    std::string llmModelPath;      // Path to DeepSeek .rkllm model
    std::string ttsBinPath;        // Path to TTS executable
    std::string ttsModelPath;      // Path to SummerTTS .bin model
    std::string asrConfigPath;     // Path to ASR config file
    // Path to KWS (keyword spotter) config file. When non-empty AND the
    // referenced config has kws_enabled=true, VoiceManager enables a two-stage
    // wake-word-gated ASR flow: KWS continuously monitors the microphone, and
    // only on a hit does ASR start a short recognition window. This avoids
    // running ASR 24/7 (lower CPU, fewer false dispatches).
    //
    // When empty or kws_enabled=false, VoiceManager falls back to the legacy
    // single-stage continuous ASR flow (current behavior).
    std::string kwsConfigPath;
    std::string llmAddress;        // ZMQ address for LLM (default: tcp://localhost:6666)
    std::string ttsAddress;        // ZMQ address for TTS text (default: tcp://localhost:7777)
    std::string ttsStatusAddress;  // ZMQ address for TTS status (default: tcp://localhost:6677)
    int zmqTimeoutMs = 30000;      // ZMQ request timeout
};

/**
 * @brief Parse a boolean from a config value string.
 *
 * Accepted (case-insensitive): true/false, 1/0, yes/no, on/off.
 * Any other value yields @p defaultValue and a warning is logged.
 * Never throws.
 */
bool parseBool(const std::string &value, bool defaultValue);

/**
 * @brief Load VoiceConfig from an INI-style key=value file.
 *
 * Missing file returns a default-constructed VoiceConfig (all disabled)
 * and logs a warning. Malformed lines are skipped. Illegal boolean values
 * fall back to safe defaults. Never throws.
 */
VoiceConfig loadVoiceConfig(const std::string &path);

/**
 * @brief Dual-path routing decision (section 六).
 *
 * Pure decision logic, unit-testable without Qt. MainWindow::
 * onOfflineAsrTextRecognized() mirrors this exactly.
 *
 * @param commandMatched   true when a fixed cabin command matched.
 * @param shouldFallback   value of unknown_command_fallback_to_llm.
 * @param llmAvailable     true only when LLM is enabled AND running AND
 *                         ZMQ client is ready.
 *
 * Invariants enforced:
 *  - commandMatched=true NEVER routes to LLM, regardless of other inputs.
 *  - LLM is only contacted when commandMatched=false && shouldFallback &&
 *    llmAvailable.
 */
enum class VoiceRoute {
    DirectCommand,   // fixed command matched, execute GUI
    SendToLlm,       // unmatched, fallback to LLM
    LogUnavailable   // unmatched, LLM not available/disabled
};
VoiceRoute decideVoiceRoute(bool commandMatched, bool shouldFallback, bool llmAvailable);

} // namespace voice

#endif // VOICE_CONFIG_H
