/**
 * @file VoiceManager.cpp
 * @brief Voice pipeline manager implementation.
 *
 * ASR/LLM/TTS are independently toggleable. ASR runs in-process on a
 * dedicated thread; LLM/TTS run as separate QProcess children and are
 * contacted via ZMQ. Fixed-command routing is intentionally NOT done
 * here: VoiceManager only emits asrTextRecognized() and lets the upper
 * layer (MainWindow) decide direct-command vs. LLM fallback.
 */

#include "VoiceManager.h"
#include "AsrEngine.h"
#include "KwsEngine.h"
#include "voice_config.h"
#include "logmanager.h"
#include "ZmqClient.h"

#include <QCoreApplication>
#include <alsa/asoundlib.h>
#include <poll.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

static const char *MODULE = "VoiceManager";

// Convert a ZMQ connect address (tcp://host:port) to a bind address (tcp://*:port).
// Used when passing addresses to child processes that bind the socket.
static std::string toBindAddress(const std::string &addr)
{
    auto pos = addr.find("tcp://");
    if (pos == std::string::npos) {
        return addr;
    }
    auto colonPos = addr.rfind(':');
    if (colonPos == std::string::npos || colonPos < pos + 6) {
        return addr;
    }
    return "tcp://*" + addr.substr(colonPos);
}

namespace voice {

VoiceManager::VoiceManager(QObject *parent)
    : QObject(parent)
{
}

VoiceManager::~VoiceManager()
{
    // Set destroying flag BEFORE deinit() so that signal emission is
    // suppressed. Qt destroys children (us) before parents (MainWindow),
    // so receivers of our signals may already be in an invalid state
    // if deinit() is called during destruction.
    m_destroying = true;
    deinit();
}

bool VoiceManager::init(const VoiceConfig &config)
{
    // Idempotency: already-initialized -> success (section 三).
    if (m_ready) {
        VLOG_WARN(MODULE, "already initialized");
        return true;
    }

    m_config = config;

    // voice_enabled=false -> success but not ready (no voice path).
    if (!m_config.enabled) {
        VLOG_INFO(MODULE, "voice interaction disabled in config");
        return true;
    }

    // --- ASR submodule (independent of LLM/TTS) ---
    if (!m_config.asrConfigPath.empty()) {
        AsrConfig asrConfig = AsrEngine::loadConfig(m_config.asrConfigPath);
        if (asrConfig.enabled) {
            m_asrEngine = std::make_unique<AsrEngine>();
            if (m_asrEngine->init(asrConfig)) {
                m_asrReady = true;
                VLOG_INFO(MODULE, "ASR engine initialized");
            } else {
                VLOG_WARN(MODULE, "ASR engine init failed; continuing without ASR");
                m_asrEngine.reset();
            }
        } else {
            VLOG_INFO(MODULE, "ASR disabled in asr config");
        }
    }

    // --- KWS submodule (optional, gates ASR into two-stage flow) ---
    // When enabled, asrThreadFunc alternates between KWS_LISTEN (continuous
    // wake-word monitoring) and ASR_WINDOW (short recognition after hit).
    // When disabled or unavailable, the legacy continuous-ASR flow is used.
    // KWS requires ASR: if ASR failed, KWS is skipped (no target for the
    // post-wake-word recognition window).
    if (m_asrReady && !m_config.kwsConfigPath.empty()) {
        KwsConfig kwsConfig = KwsEngine::loadConfig(m_config.kwsConfigPath);
        if (kwsConfig.enabled) {
            m_kwsEngine = std::make_unique<KwsEngine>();
            if (m_kwsEngine->init(kwsConfig)) {
                m_kwsReady = true;
                VLOG_INFO(MODULE, "KWS engine initialized (two-stage flow active)");
            } else {
                VLOG_WARN(MODULE,
                          "KWS engine init failed; falling back to continuous ASR");
                m_kwsEngine.reset();
            }
        } else {
            VLOG_INFO(MODULE, "KWS disabled in kws config (continuous ASR flow)");
        }
    } else if (m_asrReady) {
        VLOG_INFO(MODULE, "KWS config path not set; continuous ASR flow");
    }

    // --- LLM submodule (optional, never auto-starts unless llmEnabled) ---
    if (m_config.llmEnabled) {
        if (startLlmProcess()) {
            m_llmStarted = true;
            // Create ZMQ client only when LLM process is actually running.
            try {
                m_llmClient = new zmq_component::ZmqClient(m_config.llmAddress);
                m_llmClient->setTimeout(m_config.zmqTimeoutMs);
                m_llmClientReady = true;
            } catch (const std::exception &e) {
                VLOG_ERROR(MODULE, "ZMQ client creation failed: %s", e.what());
                delete m_llmClient;
                m_llmClient = nullptr;
                stopLlmProcess();
                m_llmStarted = false;
                emit voiceError("LLM ZMQ client creation failed");
            }
        } else {
            VLOG_WARN(MODULE, "LLM process failed to start; LLM path disabled");
            emit voiceError("Failed to start LLM service");
        }
    } else {
        VLOG_INFO(MODULE, "LLM disabled");
    }

    // --- TTS submodule (optional, never auto-starts unless ttsEnabled) ---
    if (m_config.ttsEnabled) {
        if (startTtsProcess()) {
            m_ttsStarted = true;
            // Create ZMQ client for direct TTS feedback (command path).
            // This lets fixed-command matches send short announcements
            // ("已为您打开地图") without going through the LLM. The client
            // targets the TTS text port (7777); the status port (6677) is
            // not used in direct mode because tts_service treats a text
            // message containing "END" as a complete utterance.
            if (m_config.ttsFeedbackForCommand && !m_config.ttsAddress.empty()) {
                try {
                    m_ttsClient = new zmq_component::ZmqClient(m_config.ttsAddress);
                    m_ttsClient->setTimeout(m_config.zmqTimeoutMs);
                    m_ttsClientReady = true;
                    VLOG_INFO(MODULE, "TTS direct-feedback client ready (addr=%s)",
                              m_config.ttsAddress.c_str());
                } catch (const std::exception &e) {
                    VLOG_ERROR(MODULE, "TTS ZMQ client creation failed: %s", e.what());
                    delete m_ttsClient;
                    m_ttsClient = nullptr;
                    emit voiceError("TTS ZMQ client creation failed");
                }
            }
        } else {
            VLOG_WARN(MODULE, "TTS process failed to start; TTS path disabled");
            emit voiceError("Failed to start TTS service");
        }
    } else {
        VLOG_INFO(MODULE, "TTS disabled");
    }

    // --- Start ASR capture thread if ASR is ready ---
    // ASR does not depend on LLM/TTS: even with both LLM and TTS disabled,
    // the offline ASR -> fixed-command -> GUI path must work.
    if (m_asrReady) {
        startAsrThread();
    }

    // isReady: at least one usable voice path exists.
    //  - ASR + direct command  -> ready
    //  - LLM available         -> ready
    //  - all unavailable       -> not ready
    if (m_asrReady || isLlmAvailable()) {
        m_ready = true;
        if (!m_destroying.load()) {
            emit voiceStatusChanged("Voice pipeline ready");
        }
        VLOG_INFO(MODULE,
                  "voice pipeline initialized (asr=%d kws=%d llm=%d tts=%d direct=%d)",
                  m_asrReady ? 1 : 0,
                  m_kwsReady ? 1 : 0,
                  m_llmStarted ? 1 : 0,
                  m_ttsStarted ? 1 : 0,
                  m_config.directCommandEnabled ? 1 : 0);
    } else {
        VLOG_WARN(MODULE, "no usable voice path available");
        if (!m_destroying.load()) {
            emit voiceStatusChanged("Voice pipeline unavailable");
        }
    }

    // init() returns true as long as it did not hit a fatal internal error.
    // Callers distinguish real readiness via isReady().
    return true;
}

void VoiceManager::deinit()
{
    // Idempotency: deinit when nothing was started is a no-op (section 三).
    // We check individual flags instead of m_ready so that a partially
    // initialized state (e.g. ASR ok but LLM failed) still cleans up.

    stopAsrThread();

    joinWorkerThreads();

    if (m_llmClientReady) {
        delete m_llmClient;
        m_llmClient = nullptr;
        m_llmClientReady = false;
    }

    if (m_ttsClientReady) {
        delete m_ttsClient;
        m_ttsClient = nullptr;
        m_ttsClientReady = false;
    }

    if (m_llmStarted) {
        stopLlmProcess();
        m_llmStarted = false;
    }

    if (m_ttsStarted) {
        stopTtsProcess();
        m_ttsStarted = false;
    }

    if (m_asrReady) {
        m_asrEngine.reset();
        m_asrReady = false;
    }

    // KWS cleanup: symmetric with ASR. The ASR thread was already stopped
    // by stopAsrThread() above, so m_kwsEngine is no longer accessed from
    // the worker thread. Resetting here ensures the sherpa-onnx KeywordSpotter
    // handle is released before the ASR engine (which owns the shared
    // OnnxRuntime environment) is destroyed.
    if (m_kwsReady) {
        m_kwsEngine.reset();
        m_kwsReady = false;
        m_kwsStage = KwsStage::KwsListen;
    }

    if (m_ready) {
        m_ready = false;
        // Suppress signal emission during destruction: receivers (e.g.
        // MainWindow) may already be destroyed. Qt destroys children
        // (VoiceManager) before parents (MainWindow).
        if (!m_destroying) {
            emit voiceStatusChanged("Voice pipeline stopped");
        }
        VLOG_INFO(MODULE, "voice pipeline deinitialized");
    }
}

bool VoiceManager::isReady() const
{
    return m_ready;
}

bool VoiceManager::isAsrReady() const
{
    return m_asrReady;
}

bool VoiceManager::isKwsReady() const
{
    // KWS readiness is meaningful only when ASR is also ready: a KWS hit
    // needs ASR to actually recognize the post-wake-word utterance.
    // During deinit we clear m_kwsReady before resetting m_kwsEngine, so
    // this read is safe for cross-thread observation.
    return m_kwsReady && m_asrReady;
}

bool VoiceManager::isLlmAvailable() const
{
    // LLM is only callable when: explicitly enabled, process running,
    // and ZMQ client ready. All three must hold.
    return m_config.llmEnabled && m_llmStarted && m_llmClientReady;
}

bool VoiceManager::isTtsAvailable() const
{
    return m_config.ttsEnabled && m_ttsStarted;
}

bool VoiceManager::isTtsFeedbackEnabled() const
{
    // Direct TTS feedback requires: feature enabled in config, TTS process
    // running, and ZMQ client successfully created. All three must hold.
    return m_config.ttsFeedbackForCommand && m_ttsStarted && m_ttsClientReady;
}

bool VoiceManager::isAsrMuted() const
{
    return m_asrMuted.load();
}

bool VoiceManager::isDirectCommandEnabled() const
{
    return m_config.directCommandEnabled;
}

bool VoiceManager::shouldFallbackUnknownCommandToLlm() const
{
    return m_config.unknownCommandFallbackToLlm;
}

void VoiceManager::sendToLlm(const std::string &text)
{
    if (!isLlmAvailable()) {
        VLOG_WARN(MODULE, "LLM not available; sendToLlm ignored");
        return;
    }

    // Capture pointers by value so the worker lambda does not dereference
    // `this->m_llmClient` after it has been deleted by deinit().
    auto *client = m_llmClient;
    int timeout = m_config.zmqTimeoutMs;

    // Run ZMQ request in a managed thread to avoid blocking GUI.
    // Thread is stored in m_workerThreads and joined in deinit().
    std::string textCopy = text;
    std::thread worker([this, client, textCopy, timeout]() {
        try {
            VLOG_INFO(MODULE, "sending to LLM: %s", textCopy.c_str());
            std::string response = client->request(textCopy);
            VLOG_INFO(MODULE, "LLM ack: %s", response.c_str());
            // Guard against emitting signals during/after destruction.
            if (!m_destroying.load()) {
                emit llmResponseReceived(
                    QString::fromStdString(textCopy),
                    QString::fromStdString(response));
            }
        } catch (const std::exception &e) {
            VLOG_ERROR(MODULE, "LLM request failed: %s", e.what());
            if (!m_destroying.load()) {
                emit voiceError(QString("LLM request failed: %1").arg(e.what()));
            }
        }
    });

    // Store thread for managed lifecycle.
    // We do NOT join old threads here — that would block the GUI thread
    // if a worker is still running a long ZMQ request. Instead, just push
    // the new worker. joinWorkerThreads() in deinit() handles all cleanup
    // using the swap-then-join pattern (no lock held during join).
    // Workers are self-contained (capture client/textCopy by value).
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_workerThreads.push_back(std::move(worker));
    }
}

void VoiceManager::sendToTts(const std::string &text)
{
    if (!isTtsFeedbackEnabled()) {
        VLOG_DEBUG(MODULE, "TTS feedback not enabled; sendToTts ignored");
        return;
    }

    // Append "END" marker so tts_service treats this as a complete
    // utterance: synthesis_worker sees "END", strips it, and sets
    // m_first_msg=true so the next LLM turn still expects a status signal.
    // Without "END" the TTS service would keep waiting for more text.
    std::string payload = text;
    payload += "END";

    // Capture pointers by value so the worker lambda does not dereference
    // `this->m_ttsClient` after it has been deleted by deinit().
    auto *client = m_ttsClient;
    int timeout = m_config.zmqTimeoutMs;
    int mute_ms = m_config.ttsFeedbackMuteMs;

    std::string textCopy = payload;
    std::thread worker([this, client, textCopy, timeout, mute_ms]() {
        // Mute ASR BEFORE sending the request. The TTS ack returns before
        // playback completes (synthesis+playback is async on the service
        // side), so we hold the mute for a configurable window AFTER the
        // ack to cover the actual playback duration. This prevents the
        // microphone from picking up TTS audio and creating an
        // ASR -> command -> TTS -> ASR echo loop.
        m_asrMuted.store(true);
        VLOG_INFO(MODULE, "ASR muted for TTS feedback (mute_ms=%d)", mute_ms);

        try {
            VLOG_INFO(MODULE, "sending to TTS: %s", textCopy.c_str());
            std::string response = client->request(textCopy);
            VLOG_INFO(MODULE, "TTS ack: %s", response.c_str());

            // Ack received: synthesis+playback runs async on tts_service.
            // Hold the mute window to cover actual playback. Short cabin
            // feedback phrases (~6 chars) typically play in ~1-2s on
            // RK3588S; the default 3000ms window is a safe over-estimate.
            if (mute_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(mute_ms));
            }
        } catch (const std::exception &e) {
            VLOG_ERROR(MODULE, "TTS request failed: %s", e.what());
            if (!m_destroying.load()) {
                emit voiceError(QString("TTS request failed: %1").arg(e.what()));
            }
        }

        m_asrMuted.store(false);
        VLOG_INFO(MODULE, "ASR unmuted after TTS feedback window");
    });

    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_workerThreads.push_back(std::move(worker));
    }
}

void VoiceManager::joinWorkerThreads()
{
    // Swap threads out under lock, then join without holding the lock.
    // This prevents deadlock if a worker thread tries to acquire
    // m_workerMutex (e.g. via a re-entrant sendToLlm).
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        threads.swap(m_workerThreads);
    }
    for (auto &t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }
}

bool VoiceManager::startLlmProcess()
{
    if (m_config.llmBinPath.empty() || m_config.llmModelPath.empty()) {
        VLOG_ERROR(MODULE, "LLM binary or model path not configured");
        return false;
    }

    m_llmProcess = new QProcess(this);

    connect(m_llmProcess, &QProcess::readyReadStandardOutput, [this]() {
        QByteArray output = m_llmProcess->readAllStandardOutput();
        VLOG_DEBUG(MODULE, "LLM stdout: %s", output.constData());
    });

    connect(m_llmProcess, &QProcess::readyReadStandardError, [this]() {
        QByteArray output = m_llmProcess->readAllStandardError();
        VLOG_DEBUG(MODULE, "LLM stderr: %s", output.constData());
    });

    connect(m_llmProcess,
            static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            [this](int exitCode, QProcess::ExitStatus status) {
                VLOG_WARN(MODULE, "LLM process exited (code=%d, status=%d)",
                          exitCode, static_cast<int>(status));
                emit voiceStatusChanged("LLM process exited");
    });

    m_llmProcess->start(QString::fromStdString(m_config.llmBinPath),
                        QStringList()
                        << QString::fromStdString(m_config.llmModelPath)
                        << QString::fromStdString(toBindAddress(m_config.llmAddress))
                        << QString::fromStdString(m_config.ttsAddress));

    if (!m_llmProcess->waitForStarted(5000)) {
        VLOG_ERROR(MODULE, "LLM process failed to start");
        delete m_llmProcess;
        m_llmProcess = nullptr;
        return false;
    }

    VLOG_INFO(MODULE, "LLM process started (pid=%lld)",
              static_cast<long long>(m_llmProcess->processId()));
    return true;
}

bool VoiceManager::startTtsProcess()
{
    if (m_config.ttsBinPath.empty() || m_config.ttsModelPath.empty()) {
        VLOG_ERROR(MODULE, "TTS binary or model path not configured");
        return false;
    }

    m_ttsProcess = new QProcess(this);

    connect(m_ttsProcess, &QProcess::readyReadStandardOutput, [this]() {
        QByteArray output = m_ttsProcess->readAllStandardOutput();
        VLOG_DEBUG(MODULE, "TTS stdout: %s", output.constData());
    });

    connect(m_ttsProcess, &QProcess::readyReadStandardError, [this]() {
        QByteArray output = m_ttsProcess->readAllStandardError();
        VLOG_DEBUG(MODULE, "TTS stderr: %s", output.constData());
    });

    connect(m_ttsProcess,
            static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            [this](int exitCode, QProcess::ExitStatus status) {
                VLOG_WARN(MODULE, "TTS process exited (code=%d, status=%d)",
                          exitCode, static_cast<int>(status));
                emit voiceStatusChanged("TTS process exited");
    });

    // In direct mode (LLM disabled), pass --direct so tts_service skips the
    // status-signal handshake on port 6677. The voice manager pushes complete
    // utterances (text + "END") directly to the text port (7777), so the
    // blocking wait for a status signal would never complete.
    const bool directMode = !m_config.llmEnabled;
    QStringList ttsArgs;
    ttsArgs << QString::fromStdString(m_config.ttsModelPath)
            << QString::fromStdString(toBindAddress(m_config.ttsAddress));
    if (directMode) {
        ttsArgs << QStringLiteral("--direct");
    }
    m_ttsProcess->start(QString::fromStdString(m_config.ttsBinPath), ttsArgs);

    if (!m_ttsProcess->waitForStarted(5000)) {
        VLOG_ERROR(MODULE, "TTS process failed to start");
        delete m_ttsProcess;
        m_ttsProcess = nullptr;
        return false;
    }

    // Note: processId() returns qint64; cast to long long for %lld format.
    VLOG_INFO(MODULE, "TTS process started (pid=%lld, direct=%d)",
              static_cast<long long>(m_ttsProcess->processId()), directMode ? 1 : 0);
    return true;
}

void VoiceManager::stopLlmProcess()
{
    if (!m_llmProcess) {
        return;
    }

    VLOG_INFO(MODULE, "stopping LLM process");
    m_llmProcess->terminate();
    if (!m_llmProcess->waitForFinished(3000)) {
        m_llmProcess->kill();
        m_llmProcess->waitForFinished(1000);
    }

    delete m_llmProcess;
    m_llmProcess = nullptr;
}

void VoiceManager::stopTtsProcess()
{
    if (!m_ttsProcess) {
        return;
    }

    VLOG_INFO(MODULE, "stopping TTS process");
    m_ttsProcess->terminate();
    if (!m_ttsProcess->waitForFinished(3000)) {
        m_ttsProcess->kill();
        m_ttsProcess->waitForFinished(1000);
    }

    delete m_ttsProcess;
    m_ttsProcess = nullptr;
}

void VoiceManager::startAsrThread()
{
    m_asrRunning = true;
    m_asrThread = std::thread(&VoiceManager::asrThreadFunc, this);
    VLOG_INFO(MODULE, "ASR thread started");
}

void VoiceManager::stopAsrThread()
{
    if (!m_asrRunning.load() && !m_asrThread.joinable()) {
        return;
    }

    // Clear the mute flag so the ASR loop is not stuck discarding audio
    // when we want it to exit. The loop checks m_asrRunning every 200ms
    // via poll() timeout, so it will exit promptly regardless of mute state.
    m_asrMuted.store(false);
    m_asrRunning = false;
    if (m_asrThread.joinable()) {
        m_asrThread.join();
    }
    VLOG_INFO(MODULE, "ASR thread stopped");
}

void VoiceManager::asrThreadFunc()
{
    // Load ASR config to get ALSA device name.
    // NOTE: asrConfig must live until after snd_pcm_open; use std::string to
    //       avoid dangling pointer to temporary's internal buffer.
    AsrConfig asrConfig = AsrEngine::loadConfig(m_config.asrConfigPath);
    std::string deviceName = asrConfig.alsaDevice.empty() ? "plughw:0,0" : asrConfig.alsaDevice;

    // Open ALSA capture device.
    snd_pcm_t *capture = nullptr;
    int err = snd_pcm_open(&capture, deviceName.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        VLOG_ERROR(MODULE, "ALSA open failed: %s (device=%s)", snd_strerror(err), deviceName.c_str());
        return;
    }

    snd_pcm_hw_params_t *hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(capture, hw_params);
    snd_pcm_hw_params_set_access(capture, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(capture, hw_params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(capture, hw_params, 1);

    unsigned int rate = 16000;
    snd_pcm_hw_params_set_rate_near(capture, hw_params, &rate, nullptr);
    snd_pcm_uframes_t period_size = 1024;
    snd_pcm_hw_params_set_period_size_near(capture, hw_params, &period_size, nullptr);

    err = snd_pcm_hw_params(capture, hw_params);
    if (err < 0) {
        VLOG_ERROR(MODULE, "ALSA hw_params failed: %s", snd_strerror(err));
        snd_pcm_close(capture);
        return;
    }

    VLOG_INFO(MODULE, "ALSA capture opened (device=%s, rate=%u)", deviceName.c_str(), rate);

    // Force-start the capture stream by issuing one blocking readi before
    // entering the poll loop.
    //
    // Why not snd_pcm_start(): for plughw (plugin layer) devices,
    // snd_pcm_start() returns 0 (success) but the underlying hw device
    // stays in PREPARED — plughw is lazy-start and only kicks off the hw
    // stream when the app actually pulls data. Observed on USB camera mic
    // card3: snd_pcm_start() succeeded but /proc/asound/card3/pcm0c/sub0/
    // status remained "state: PREPARED" and poll() never returned POLLIN.
    //
    // Why a blocking readi works: ALSA's auto-start rule says the first
    // snd_pcm_readi() on a PREPARED capture stream transitions it to
    // RUNNING and blocks until one period of data is available. This
    // forces both the plughw plugin and the underlying hw device to begin
    // capturing. After this priming read, poll() will subsequently return
    // POLLIN whenever a period is ready.
    //
    // Cost: one blocking read of ~64ms (1024 frames @ 16kHz). Acceptable
    // since this happens once at thread startup, before any user interaction.
    static const int kFrameSize = 1024;
    int16_t pcmBuf[kFrameSize];
    float floatBuf[kFrameSize];

    snd_pcm_sframes_t startFrames = snd_pcm_readi(capture, pcmBuf, kFrameSize);
    if (startFrames < 0) {
        startFrames = snd_pcm_recover(capture, startFrames, 0);
        if (startFrames < 0) {
            VLOG_ERROR(MODULE, "ALSA priming read failed: %s", snd_strerror(startFrames));
            snd_pcm_close(capture);
            return;
        }
    }
    VLOG_INFO(MODULE, "ALSA stream primed (first readi=%ld frames)", static_cast<long>(startFrames));

    // Get poll file descriptors for ALSA capture.
    // Using poll() with timeout instead of blocking snd_pcm_readi so that
    // the thread can check m_asrRunning every 200ms and exit promptly
    // when stopAsrThread() is called. Without this, snd_pcm_readi blocks
    // indefinitely and join() in stopAsrThread() hangs.
    struct pollfd pfd;
    int nfds = snd_pcm_poll_descriptors(capture, &pfd, 1);
    if (nfds < 0) {
        VLOG_ERROR(MODULE, "ALSA poll_descriptors failed: %s", snd_strerror(nfds));
        snd_pcm_close(capture);
        return;
    }

    // PCM buffer declared above (before the priming read) — reused in loop.

    // Track mute state transitions so we can reset the ASR stream when
    // muting starts. This discards any partial recognition that might
    // contain TTS echo captured before the mute flag took effect.
    // reset() is called from this ASR thread only (not the TTS worker
    // thread) to avoid concurrent access to the sherpa-onnx stream.
    bool wasMuted = false;

    while (m_asrRunning.load()) {
        // Wait for data with 200ms timeout so we can check m_asrRunning.
        int pollRet = poll(&pfd, 1, 200);
        if (pollRet < 0) {
            if (errno == EINTR) continue;
            VLOG_ERROR(MODULE, "ALSA poll error: %s", strerror(errno));
            break;
        }
        if (pollRet == 0) {
            // Timeout — no data yet, loop back to check m_asrRunning.
            continue;
        }
        if (!(pfd.revents & POLLIN)) {
            continue;
        }

        // Read PCM from microphone.
        snd_pcm_sframes_t frames = snd_pcm_readi(capture, pcmBuf, kFrameSize);
        if (frames < 0) {
            frames = snd_pcm_recover(capture, frames, 0);
            if (frames < 0) {
                VLOG_ERROR(MODULE, "ALSA read error: %s", snd_strerror(frames));
                break;
            }
            continue;
        }

        // TTS echo suppression: when muted, drain ALSA buffer (above) to
        // prevent overrun, but discard the audio instead of feeding the
        // active engine. On the false->true transition, reset whichever
        // engine is currently consuming audio (KWS during KWS_LISTEN, ASR
        // during ASR_WINDOW) to clear any partial state that might contain
        // TTS audio captured before the mute flag took effect.
        // reset() is called from this ASR thread only (not the TTS worker
        // thread) to avoid concurrent access to the sherpa-onnx stream.
        const bool muted = m_asrMuted.load();
        if (muted) {
            if (!wasMuted) {
                if (m_kwsReady) {
                    if (m_kwsStage == KwsStage::KwsListen) {
                        m_kwsEngine->reset();
                    } else {
                        m_asrEngine->reset();
                    }
                } else {
                    m_asrEngine->reset();
                }
                VLOG_INFO(MODULE, "engine reset (muted for TTS echo suppression)");
                wasMuted = true;
            }
            continue;
        }
        if (wasMuted) {
            wasMuted = false;
            VLOG_INFO(MODULE, "ASR unmuted, resuming recognition");
        }

        // Convert int16 to float [-1, 1].
        for (int i = 0; i < frames; ++i) {
            floatBuf[i] = static_cast<float>(pcmBuf[i]) / 32768.0f;
        }

        if (m_kwsReady) {
            // Two-stage KWS-gated flow: alternate between KWS_LISTEN and
            // ASR_WINDOW based on m_kwsStage. Helper returns true when
            // the state machine should transition.
            if (m_kwsStage == KwsStage::KwsListen) {
                if (runKwsListenLoop(floatBuf, static_cast<int32_t>(frames))) {
                    m_kwsStage = KwsStage::AsrWindow;
                    m_kwsAsrWindowStartMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
                    VLOG_INFO(MODULE, "KWS -> ASR_WINDOW (wake-word detected)");
                }
            } else {
                if (runAsrWindowLoop(floatBuf, static_cast<int32_t>(frames))) {
                    m_kwsStage = KwsStage::KwsListen;
                    // Reset KWS stream on transition back: the KWS_LISTEN
                    // loop should start fresh, without any leftover context
                    // from audio captured during ASR_WINDOW.
                    m_kwsEngine->reset();
                    VLOG_INFO(MODULE, "ASR_WINDOW -> KWS_LISTEN");
                }
            }
        } else {
            // Legacy continuous-ASR flow: feed every PCM chunk to ASR.
            // Used when KWS is disabled or unavailable.
            m_asrEngine->acceptWaveform(floatBuf, static_cast<int32_t>(frames));

            while (m_asrEngine->isReadyToDecode()) {
                m_asrEngine->decode();
            }

            if (m_asrEngine->isEndpoint()) {
                std::string text = m_asrEngine->getResult();
                m_asrEngine->reset();

                if (!text.empty()) {
                    VLOG_INFO(MODULE, "ASR recognized: %s", text.c_str());
                    // VoiceManager ONLY emits the recognized text. The upper
                    // layer (MainWindow) routes to direct command or LLM.
                    // Unconditional sendToLlm() was removed per section 四.
                    emit asrTextRecognized(QString::fromStdString(text));
                }
            }
        }
    }

    snd_pcm_close(capture);
    VLOG_INFO(MODULE, "ALSA capture closed");
}

bool VoiceManager::runKwsListenLoop(const float *samples, int32_t numSamples)
{
    // Feed PCM to KWS engine.
    m_kwsEngine->acceptWaveform(samples, numSamples);

    // Drain decode queue (KWS may need multiple decode calls per chunk).
    while (m_kwsEngine->isReadyToDecode()) {
        m_kwsEngine->decode();
    }

    // Check for wake-word hit.
    // KWS has no endpoint concept: getResult() returns a non-empty keyword
    // when a wake word is matched. After a hit, reset() MUST be called
    // before the next decode cycle, otherwise the engine keeps returning
    // the same hit every cycle (confirmed behavior in sherpa-onnx 1.13.3:
    // SherpaOnnxResetKeywordStream flushes the matched state).
    std::string keyword = m_kwsEngine->getResult();
    if (keyword.empty()) {
        return false;  // no hit, stay in KWS_LISTEN
    }

    // Critical: reset KWS stream immediately to clear the matched state
    // and prevent repeat firing while ASR_WINDOW is active.
    m_kwsEngine->reset();

    VLOG_INFO(MODULE, "KWS hit: %s", keyword.c_str());
    if (!m_destroying.load()) {
        emit keywordDetected(QString::fromStdString(keyword));
    }

    // Reset ASR engine before opening ASR_WINDOW: any audio captured
    // during KWS_LISTEN must NOT leak into the post-wake-word recognition
    // window. ASR was idle during KWS_LISTEN but may have residual state
    // from the previous ASR_WINDOW transition.
    m_asrEngine->reset();
    return true;  // transition to ASR_WINDOW
}

bool VoiceManager::runAsrWindowLoop(const float *samples, int32_t numSamples)
{
    // Feed PCM to ASR engine.
    m_asrEngine->acceptWaveform(samples, numSamples);

    // Drain decode queue.
    while (m_asrEngine->isReadyToDecode()) {
        m_asrEngine->decode();
    }

    // Check endpoint (user stopped speaking).
    if (m_asrEngine->isEndpoint()) {
        std::string text = m_asrEngine->getResult();
        m_asrEngine->reset();

        if (!text.empty()) {
            VLOG_INFO(MODULE, "ASR recognized (post-wake-word): %s", text.c_str());
            if (!m_destroying.load()) {
                // Reuse the same asrTextRecognized() signal as the legacy
                // continuous-ASR flow. The upper layer (MainWindow) does
                // not need to know whether the text came from a KWS-gated
                // window or continuous ASR — routing is identical.
                emit asrTextRecognized(QString::fromStdString(text));
            }
        }
        return true;  // transition back to KWS_LISTEN
    }

    // Hard timeout: a noisy environment can keep ASR in non-endpoint state
    // indefinitely. Bail out after kAsrWindowTimeoutMs so the user can
    // re-trigger KWS with another wake word. 6s is long enough for typical
    // cabin commands ("打开地图") but short enough to recover from a
    // misfired wake word in a quiet car.
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (nowMs - m_kwsAsrWindowStartMs > kAsrWindowTimeoutMs) {
        VLOG_INFO(MODULE, "ASR window timeout (%d ms), returning to KWS_LISTEN",
                  kAsrWindowTimeoutMs);
        m_asrEngine->reset();
        return true;
    }

    return false;  // stay in ASR_WINDOW
}

VoiceConfig VoiceManager::loadConfig(const std::string &path)
{
    // Delegate to the Qt-free, unit-testable loader.
    return loadVoiceConfig(path);
}

} // namespace voice
