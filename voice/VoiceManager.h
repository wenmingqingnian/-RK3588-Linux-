#ifndef VOICE_MANAGER_H
#define VOICE_MANAGER_H

/**
 * @file VoiceManager.h
 * @brief Manages the offline voice interaction pipeline.
 *
 * Orchestrates three independently-toggleable submodules:
 *   - ASR  (in-process via sherpa-onnx, ALSA microphone)
 *   - LLM  (separate process via ZMQ, optional)
 *   - TTS  (separate process via ZMQ, optional)
 *
 * Dual-path routing (section 六 of the implementation spec):
 *   1. ASR text -> vehiclecommand keyword match -> direct GUI action.
 *      Fixed-command matches NEVER go to the LLM.
 *   2. Unmatched ASR text -> optional LLM fallback when llmEnabled &&
 *      unknownCommandFallbackToLlm && LLM process is actually running.
 *
 * VoiceManager only emits asrTextRecognized(QString); the upper-layer
 * command router (MainWindow) decides direct-command vs. LLM fallback.
 *
 * Threading:
 *   - ASR runs on a dedicated thread (m_asrThread) reading from ALSA.
 *   - LLM communication is async via a managed worker thread (non-blocking
 *     to GUI). Worker threads are joined in deinit().
 *   - VoiceManager itself is a QObject living on the GUI main thread.
 */

#include <QObject>
#include <QProcess>
#include <QString>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

#include "voice_config.h"

namespace zmq_component {
class ZmqClient;
}

namespace voice {

class AsrEngine;
class KwsEngine;

class VoiceManager : public QObject {
    Q_OBJECT

public:
    explicit VoiceManager(QObject *parent = nullptr);
    ~VoiceManager();

    /**
     * @brief Initialize enabled submodules independently.
     *
     * - voice_enabled=false: returns true, isReady() returns false.
     * - voice_enabled=true: initializes ASR/LLM/TTS according to their
     *   respective enable flags. A submodule failure does NOT block the
     *   others. ASR + direct command alone yields isReady()==true.
     *
     * Safe defaults: LLM/TTS never auto-start unless explicitly enabled.
     *
     * When kws_config_path is set and the referenced config has
     * kws_enabled=true, a KwsEngine is initialized alongside AsrEngine
     * and the ASR thread runs a two-stage flow (KWS gate ASR). When KWS
     * is disabled or unavailable, the legacy continuous-ASR flow is used.
     */
    bool init(const VoiceConfig &config);
    void deinit();
    bool isReady() const;

    // --- Independent submodule status (section 三) ---
    bool isAsrReady() const;
    bool isLlmAvailable() const;
    bool isTtsAvailable() const;
    bool isTtsFeedbackEnabled() const;
    bool isDirectCommandEnabled() const;
    bool shouldFallbackUnknownCommandToLlm() const;
    bool isKwsReady() const;  // true when KWS engine initialized and active

    /**
     * @brief Returns true while ASR capture is muted for TTS echo suppression.
     *
     * When true, the ASR thread still drains the ALSA capture buffer (to
     * prevent overrun) but discards the audio instead of feeding the ASR
     * engine. This prevents the microphone from picking up TTS playback
     * and creating an ASR -> command -> TTS -> ASR echo loop.
     *
     * Muted windows are opened by sendToTts() and closed after a
     * configurable delay (tts_feedback_mute_ms, default 3000 ms) following
     * the TTS ack. The ASR stream is reset() at the start of each mute
     * window to discard any partial recognition that might contain TTS
     * echo.
     */
    bool isAsrMuted() const;

    /**
     * @brief Send text to LLM for processing (async, non-blocking).
     *        No-op when LLM is not available. Response forwarded via
     *        llmResponseReceived signal.
     * @param text The recognized speech text or user query
     */
    void sendToLlm(const std::string &text);

    /**
     * @brief Send feedback text to TTS for direct announcement (async,
     *        non-blocking). Used by fixed-command path to give voice feedback
     *        (e.g. "已为您打开地图") without going through the LLM.
     *        No-op when TTS feedback is disabled or TTS service is unavailable.
     *        The text is appended with "END" so the TTS service treats it as
     *        a complete utterance and signals playback completion.
     * @param text The feedback text to synthesize and play
     */
    void sendToTts(const std::string &text);

    static VoiceConfig loadConfig(const std::string &path);

signals:
    void llmResponseReceived(const QString &userText, const QString &llmText);
    void asrTextRecognized(const QString &text);

    /**
     * @brief Emitted when a KWS wake word is detected.
     *
     * Fires before the ASR short window opens; MainWindow can use this to
     * give voice feedback ("我在") or update a UI indicator. The recognized
     * text from the post-wake-word ASR window is still delivered via
     * asrTextRecognized() (same path as the legacy continuous-ASR flow).
     *
     * Suppressed during destruction (m_destroying).
     */
    void keywordDetected(const QString &keyword);
    void voiceError(const QString &error);
    void voiceStatusChanged(const QString &status);

private:
    bool startLlmProcess();
    bool startTtsProcess();
    void stopLlmProcess();
    void stopTtsProcess();
    void startAsrThread();
    void stopAsrThread();
    void asrThreadFunc();

    // --- Two-stage KWS-gated ASR flow (asrThreadFunc helpers) ---
    // When KWS is active, asrThreadFunc alternates between two states:
    //   1. KWS_LISTEN: feed PCM to KwsEngine; on hit -> ASR_WINDOW
    //   2. ASR_WINDOW: feed PCM to AsrEngine; on endpoint/timeout -> KWS_LISTEN
    //
    // Both states share one ALSA stream (no double-open). When KWS is
    // disabled, asrThreadFunc runs the legacy continuous-ASR flow only.
    //
    // Each helper processes one PCM chunk (already converted to float
    // [-1,1]) and returns true when the state machine should transition
    // to the other state. Returning false means "stay in current state".
    bool runKwsListenLoop(const float *samples, int32_t numSamples);
    bool runAsrWindowLoop(const float *samples, int32_t numSamples);

    void joinWorkerThreads();

    VoiceConfig m_config;
    QProcess *m_llmProcess = nullptr;
    QProcess *m_ttsProcess = nullptr;
    zmq_component::ZmqClient *m_llmClient = nullptr;
    zmq_component::ZmqClient *m_ttsClient = nullptr;  // direct TTS feedback (command path)
    std::unique_ptr<AsrEngine> m_asrEngine;
    std::unique_ptr<KwsEngine> m_kwsEngine;   // null when KWS disabled/unavailable
    std::thread m_asrThread;
    std::atomic<bool> m_asrRunning{false};
    std::atomic<bool> m_asrMuted{false};   // TTS echo suppression: true = discard captured audio
    std::vector<std::thread> m_workerThreads;   // managed sendToLlm/sendToTts threads
    std::mutex m_workerMutex;                    // protects m_workerThreads

    // Independent submodule state flags. Each is set only after the
    // corresponding resource is successfully initialized, and cleared
    // before the resource is released. deinit() only stops submodules
    // that were actually started.
    bool m_asrReady = false;
    bool m_kwsReady = false;       // KWS engine initialized; two-stage flow active
    bool m_llmStarted = false;     // LLM QProcess running
    bool m_ttsStarted = false;     // TTS QProcess running
    bool m_llmClientReady = false; // ZMQ client created
    bool m_ttsClientReady = false; // TTS direct-feedback ZMQ client created
    bool m_ready = false;          // at least one usable voice path exists
    std::atomic<bool> m_destroying{false}; // true during ~VoiceManager(); suppress signals

    // --- Two-stage KWS-gated ASR flow state ---
    // Only meaningful while m_asrThread is running and m_kwsReady==true.
    // m_kwsAsrWindowStartMs records the steady_clock time point (as ms
    // since epoch) when ASR_WINDOW was entered; used by runAsrWindowLoop
    // to enforce a hard timeout (kAsrWindowTimeoutMs) so a noisy env
    // can't pin the pipeline in ASR mode forever.
    enum class KwsStage {
        KwsListen,   // continuous wake-word monitoring
        AsrWindow    // short recognition window after a wake-word hit
    };
    KwsStage m_kwsStage = KwsStage::KwsListen;
    long long m_kwsAsrWindowStartMs = 0;  // std::chrono::steady_clock ms since epoch
    static constexpr int kAsrWindowTimeoutMs = 6000;  // hard cap for ASR_WINDOW
};

} // namespace voice

#endif // VOICE_MANAGER_H
