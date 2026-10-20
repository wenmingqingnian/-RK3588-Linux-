#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "vehiclecommand.h"
#include "logmanager.h"
#include "mpu6050.h"
#include "vehicleioinput.h"

#include <atomic>
#include <cstdlib>
#include <string>

static const char *MODULE = "mainwindow";

static std::string getEnvOr(const char *envName, const std::string &fallback)
{
    const char *val = std::getenv(envName);
    if (val != nullptr && val[0] != '\0') {
        return std::string(val);
    }
    return fallback;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    VLOG_INFO(MODULE, "initializing main window");

    QVBoxLayout *layout = new QVBoxLayout;
    layout->addWidget(&clock);
    ui->clock_Widget->setLayout(layout);
    ui->stackedWidget->installEventFilter(this);

    // Load all module configurations (device/camera/asr/rknn/voice/rtsp).
    loadAllConfigs();

    voiceManager = new voice::VoiceManager(this);
    if (voiceManager->init(voiceConfig)) {
        if (voiceManager->isReady()) {
            VLOG_INFO(MODULE, "voice pipeline initialized");
        } else {
            VLOG_INFO(MODULE, "voice pipeline disabled in config");
        }
    } else {
        VLOG_WARN(MODULE, "voice pipeline init failed");
    }

    // Connect offline ASR text signal for dual-path routing (section 五/六).
    // AutoConnection: signal is emitted from VoiceManager's ASR worker thread,
    // the slot runs on the GUI main thread (queued).
    connect(voiceManager,
            &voice::VoiceManager::asrTextRecognized,
            this,
            &MainWindow::onOfflineAsrTextRecognized);

    // Connect KWS wake-word signal for voice feedback.
    // Fires when KWS detects a wake word, before the ASR recognition
    // window opens. The slot plays a short "我在" announcement (when TTS
    // feedback is enabled) so the user knows the system is listening.
    connect(voiceManager,
            &voice::VoiceManager::keywordDetected,
            this,
            &MainWindow::onKeywordDetected);

    dht11Reader = new Dht11Reader(deviceConfig.dht11, this);

    mpu6050Reader = new Mpu6050Reader(deviceConfig.mpu6050, this);

    baiduMap = new BaiduMap(this);
    monitor = Monitor::getInstance(deviceConfig.ap3216,
                                   cameraConfig,
                                   rearCameraConfig,
                                   rknnConfig,
                                   rtspConfig);
    musicPlayer = new MusicPlayer(this);
    weather = new Weather(this);
    windows.append(baiduMap);
    windows.append(monitor);
    windows.append(weather);
    windows.append(musicPlayer);
    time = new QTimer(this);
    time->setInterval(500);
    time->start();

    // Initialize buzzer device. When disabled in config, openDevice is a
    // no-op success. When enabled, opens the GPIO line handle and sets
    // the initial level to silent (alarm off).
    buzzer = new vehicledevice::BuzzerDevice(deviceConfig.buzzer);
    {
        std::string buzzerError;
        if (!buzzer->openDevice(&buzzerError)) {
            VLOG_WARN(MODULE, "buzzer open failed: %s", buzzerError.c_str());
        } else if (deviceConfig.buzzer.enabled) {
            VLOG_INFO(MODULE, "buzzer initialized and ready");
        } else {
            VLOG_INFO(MODULE, "buzzer disabled in config");
        }
    }

    alarmCycleTimer = new QTimer(this);
    alarmCycleTimer->setInterval(1000);
    alarmPulseOffTimer = new QTimer(this);
    alarmPulseOffTimer->setInterval(200);
    alarmPulseOffTimer->setSingleShot(true);
    connect(alarmCycleTimer, &QTimer::timeout,
            this, &MainWindow::onAlarmCycle);
    connect(alarmPulseOffTimer, &QTimer::timeout,
            this, &MainWindow::onAlarmPulseOff);
    connect(monitor, &Monitor::dangerStateChanged,
            this, &MainWindow::onDangerStateChanged);

    // Vehicle-IO business integration:
    // REVERSE press opens Monitor; release deliberately does not close it.
    // ACC changes are logged.
    // BUTTON press generates a non-blocking 200 ms buzzer pulse.
    vehicleIoInput = new VehicleIoInput(this);

    connect(vehicleIoInput,
            &VehicleIoInput::deviceConnected,
            this,
            [](const QString &devicePath) {
                VLOG_INFO(MODULE,
                          "vehicle-io connected: %s",
                          devicePath.toUtf8().constData());
            });

    connect(vehicleIoInput,
            &VehicleIoInput::deviceDisconnected,
            this,
            [](const QString &devicePath) {
                VLOG_WARN(MODULE,
                          "vehicle-io disconnected: %s",
                          devicePath.toUtf8().constData());
            });

    connect(vehicleIoInput,
            &VehicleIoInput::errorOccurred,
            this,
            [](const QString &message) {
                VLOG_WARN(MODULE,
                          "vehicle-io error: %s",
                          message.toUtf8().constData());
            });

    connect(vehicleIoInput,
            &VehicleIoInput::reverseChanged,
            this,
            [this](bool active) {
                VLOG_INFO(MODULE,
                          "vehicle-io REVERSE=%s",
                          active ? "ON" : "OFF");

                if (!monitor) {
                    VLOG_WARN(MODULE,
                              "REVERSE event ignored: Monitor unavailable");
                    return;
                }

                // CameraRouter owns only the local GUI selection. Front
                // capture, RKNN and RTSP continue regardless of reverse state.
                monitor->setReverseActive(active);

                if (!active) {
                    VLOG_INFO(MODULE,
                              "REVERSE released; Monitor display returned Front");
                    return;
                }

                on_pBtn_Monitor_clicked();

                VLOG_INFO(MODULE,
                          "REVERSE press opened Monitor page; Rear requested");
            });

    connect(vehicleIoInput,
            &VehicleIoInput::accChanged,
            this,
            [](bool active) {
                VLOG_INFO(MODULE,
                          "vehicle-io ACC=%s",
                          active ? "ON" : "OFF");
            });

    QTimer *vehicleButtonBuzzerTimer = new QTimer(this);
    vehicleButtonBuzzerTimer->setInterval(200);
    vehicleButtonBuzzerTimer->setSingleShot(true);

    connect(vehicleButtonBuzzerTimer,
            &QTimer::timeout,
            this,
            [this]() {
                if (setBuzzerAlarm(false)) {
                    VLOG_INFO(MODULE,
                              "vehicle-io BUTTON buzzer pulse finished");
                }
            });

    connect(vehicleIoInput,
            &VehicleIoInput::buttonChanged,
            this,
            [this, vehicleButtonBuzzerTimer](bool active) {
                VLOG_INFO(MODULE,
                          "vehicle-io BUTTON=%s",
                          active ? "ON" : "OFF");

                if (!active) {
                    return;
                }

                if (!setBuzzerAlarm(true)) {
                    VLOG_WARN(MODULE,
                              "BUTTON press could not activate buzzer");
                    return;
                }

                // start() restarts the single-shot timer if another valid
                // press arrives before the previous 200 ms pulse expires.
                vehicleButtonBuzzerTimer->start();

                VLOG_INFO(MODULE,
                          "vehicle-io BUTTON buzzer pulse started: 200 ms");
            });

    if (!vehicleIoInput->start()) {
        // start() enables background auto-scan when the device is not
        // immediately available, so this is not a fatal startup failure.
        VLOG_INFO(MODULE,
                  "vehicle-io not immediately available; auto-scan enabled");
    }

    VLOG_INFO(MODULE,
              "vehicle-io business integration initialized");


    request = new QNetworkRequest;
    networkManage = new QNetworkAccessManager(this);
    request->setHeader(QNetworkRequest::ContentTypeHeader,
                       QVariant("application/json"));
    connect(networkManage, SIGNAL(finished(QNetworkReply *)),
            this, SLOT(getSpeechResult(QNetworkReply *)));

    // Legacy online ASR conflict handling (section 七).
    // Offline ASR uses ALSA snd_pcm_open directly; the legacy path uses
    // QAudioRecorder. They MUST NOT hold the microphone at the same time.
    // When offline ASR is ready, disable the legacy path entirely.
    // When offline ASR is unavailable, keep the legacy path as a fallback.
    AsrThread = new SpeechRecognition();
    if (voiceManager->isAsrReady()) {
        VLOG_INFO(MODULE, "offline ASR ready, legacy online ASR disabled");
        legacyAsrStarted = false;
    } else if (!asrConfig.asrEnabled) {
        // Online ASR disabled by config (asr_enabled=false or placeholder
        // credentials). Do not start the legacy online path; the offline
        // command route still works for fixed commands.
        VLOG_INFO(MODULE,
                  "offline ASR unavailable and online ASR disabled by config; "
                  "legacy online ASR not started");
        legacyAsrStarted = false;
    } else {
        VLOG_INFO(MODULE,
                  "offline ASR unavailable, falling back to legacy online ASR");
        AsrThread->startSpeechRecognition();
        legacyAsrStarted = true;
    }
    connect(AsrThread, SIGNAL(RecordFinished()),
            this, SLOT(on_handleRecord()));
    connect(time, SIGNAL(timeout()),
            this, SLOT(updateTimeDisplay()));
    connect(dht11Reader, SIGNAL(updateDht11Data(QString, QString)),
            this, SLOT(updateHumidityTemp(QString, QString)));
    // DHT11 设备可用性 / 采样告警仅记日志，不触发 GUI 动作。
    // 设备不可用时 updateDht11Data 自然停止刷新，GUI 维持上一次显示。
    connect(dht11Reader, &Dht11Reader::availabilityChanged,
            this, [](bool available) {
                VLOG_INFO(MODULE, "dht11 availability changed: %s",
                          available ? "true" : "false");
            });
    connect(dht11Reader, &Dht11Reader::warning,
            this, [](const QString &message) {
                VLOG_WARN(MODULE, "dht11 warning: %s",
                          message.toUtf8().constData());
            });

    // Dht11Reader::startReading() 异步启动 worker 线程，无返回值。
    // 必须在所有 connect() **之后**调用：worker 启动后会立即调用
    // onPollTimeout()，可能在首次 timeout 就发出 availabilityChanged /
    // updateDht11Data / warning 信号；若先 startReading 后 connect，
    // 第一帧信号会丢失。
    dht11Reader->startReading();

    // MPU6050: 设备可用性 / 采样数据 / 告警仅记日志（不接入 GUI 控件，
    // 数据信号供后续危险检测/姿态监控扩展使用）。
    connect(mpu6050Reader, &Mpu6050Reader::availabilityChanged,
            this, [](bool available) {
                VLOG_INFO(MODULE, "mpu6050 availability changed: %s",
                          available ? "true" : "false");
            });
    connect(mpu6050Reader, &Mpu6050Reader::warning,
            this, [](const QString &message) {
                VLOG_WARN(MODULE, "mpu6050 warning: %s",
                          message.toUtf8().constData());
            });
    // 50 Hz 高频信号：仅在 debug 级别记日志，每 10 帧采样一次（5 Hz 日志）
    // 避免日志洪泛。GUI 控件若需要更新由后续接入决定。
    static std::atomic<int> mpu6050LogCounter{0};
    connect(mpu6050Reader, &Mpu6050Reader::updateMpu6050Data,
            this, [](double ax, double ay, double az,
                     double gx, double gy, double gz,
                     double temperature,
                     double pitch, double roll) {
                if (++mpu6050LogCounter % 10 != 0) {
                    return;
                }
                VLOG_DEBUG(MODULE,
                    "mpu6050: ax=%.2f ay=%.2f az=%.2f m/s^2 | "
                    "gx=%.1f gy=%.1f gz=%.1f deg/s | "
                    "T=%.1f°C | pitch=%.1f roll=%.1f",
                    ax, ay, az, gx, gy, gz, temperature, pitch, roll);
            });
    mpu6050Reader->startReading();

    connect(this, SIGNAL(SendCommandToMap(int)),
            baiduMap, SLOT(on_handleCommand(int)));
    connect(this, SIGNAL(SendCommandToMonitor(int)),
            monitor, SLOT(on_handleCommand(int)));
    connect(this, SIGNAL(SendCommandToMusic(int)),
            musicPlayer, SLOT(on_handleCommand(int)));

    // Test injection: inject voice command text from environment variable.
    // When VEHICLE_VOICE_TEST_TEXT is set, the text is dispatched through the
    // same offline ASR → vehiclecommand → GUI path after a 1-second delay.
    // When unset, this block has no effect on normal runtime.
    {
        const QByteArray testText = qgetenv("VEHICLE_VOICE_TEST_TEXT");
        if (!testText.isEmpty()) {
            const QString injectedText = QString::fromUtf8(testText);
            QTimer::singleShot(1000, this, [this, injectedText]() {
                VLOG_INFO(MODULE,
                          "injecting voice test text: \"%s\"",
                          injectedText.toUtf8().constData());
                onOfflineAsrTextRecognized(injectedText);
            });
        }
    }

    VLOG_INFO(MODULE, "main window initialized");
}

MainWindow::~MainWindow()
{
    if (vehicleIoInput) {
        vehicleIoInput->stop();
    }

    if (alarmCycleTimer) {
        alarmCycleTimer->stop();
    }
    if (alarmPulseOffTimer) {
        alarmPulseOffTimer->stop();
    }
    alarmManager.clear();

    if (monitor) {
        // myStop() stops active camera/device work. Monitor destruction then
        // releases the RTSP server and finalizes the active MP4 segment.
        monitor->myStop();
        Monitor::destroyInstance();
        monitor = nullptr;
    }

    // Silence the buzzer before destroying it to ensure the GPIO line is
    // left at the silent level (high for active-low) on application exit.
    if (buzzer) {
        if (buzzer->isAvailable()) {
            std::string buzzerError;
            if (!buzzer->setAlarm(false, &buzzerError)) {
                VLOG_WARN(MODULE, "buzzer silence on exit failed: %s",
                          buzzerError.c_str());
            }
        }
        delete buzzer;
        buzzer = nullptr;
    }
    // Release the legacy online ASR if it was actually started. The offline
    // VoiceManager is parented to this window and is auto-deleted by Qt
    // (its destructor calls deinit(), which is idempotent and only stops
    // submodules that were actually started).
    if (AsrThread) {
        delete AsrThread;
        AsrThread = nullptr;
    }
    // QNetworkAccessManager is parented to this and auto-deleted by Qt.
    // QNetworkRequest is not a QObject; release explicitly to avoid leak.
    delete request;
    request = nullptr;
    delete ui;
}


void MainWindow::on_pBtn_Setting_clicked()
{
    settingWindow.ScanGif();
    settingWindow.show();
}

void MainWindow::updateHumidityTemp(QString humidity, QString temp)
{
    ui->label_humi->setText(humidity);
    ui->label_temp->setText(temp);

    bool temperatureOk = false;
    const double temperatureCelsius = temp.trimmed().toDouble(&temperatureOk);
    if (!temperatureOk) {
        VLOG_WARN(MODULE, "invalid DHT11 temperature: %s",
                  temp.toUtf8().constData());
        return;
    }

    if (temperatureAlarmPolicy.update(temperatureCelsius)) {
        updateAlarmSource(vehiclealarm::AlarmSource::TemperatureHigh,
                          temperatureAlarmPolicy.isActive());
    }
}

void MainWindow::getSpeechResult(QNetworkReply *reply)
{
    VLOG_DEBUG(MODULE, "getSpeechResult: reply readable=%d", reply->isReadable());

    QByteArray content = reply->readAll();
    // Guard: if content has no trailing newline, lastIndexOf returns -1
    // and remove(-1, 1) would silently delete the last byte.
    int nlPos = content.lastIndexOf('\n');
    if (nlPos >= 0) {
        content.remove(nlPos, 1);
    }
    QJsonDocument doc = QJsonDocument::fromJson(content);
    if (!doc.isObject()) {
        VLOG_WARN(MODULE, "ASR reply is not a JSON object");
        reply->deleteLater();
        return;
    }
    QJsonObject obj = doc.object();
    QJsonArray resutls = obj.value("result").toArray();
    if (resutls.isEmpty()) {
        VLOG_WARN(MODULE, "ASR reply has empty result array");
        reply->deleteLater();
        return;
    }
    QString AsrResult = resutls.at(0).toString();
    VLOG_INFO(MODULE, "ASR result: %s", AsrResult.toUtf8().constData());

    // Reuse the shared dispatch path. Legacy online ASR never falls back to
    // the LLM here (the legacy path predates the LLM integration); unmatched
    // commands are simply logged.
    dispatchVoiceCommand(AsrResult);

    // Ensure reply is always released (Qt does not auto-delete replies
    // delivered via finished() signal; caller owns them).
    reply->deleteLater();
}

bool MainWindow::dispatchVoiceCommand(const QString &text)
{
    QByteArray commandBytes = text.toUtf8();
    vehiclecommand::CabinCommand command = vehiclecommand::parseCabinCommand(
        std::string(commandBytes.constData(), commandBytes.size()));

    if (command == vehiclecommand::CabinCommand::Unknown) {
        VLOG_INFO(MODULE,
                  "voice command unmatched: text=\"%s\"",
                  commandBytes.constData());
        return false;
    }

    VLOG_INFO(MODULE,
              "voice command matched: text=\"%s\" command=%s route=direct",
              commandBytes.constData(),
              vehiclecommand::toString(command));
    executeCabinCommand(command);
    return true;
}

// Map a parsed cabin command to a short TTS feedback phrase.
// Returns an empty string for Unknown (no feedback).
// These phrases are designed to be short so SummerTTS can synthesize them
// quickly (typically < 1 second) for a responsive voice-feedback loop.
static const char *feedbackTextForCommand(vehiclecommand::CabinCommand command)
{
    switch (command) {
    case vehiclecommand::CabinCommand::MusicShow:     return "已为您打开音乐";
    case vehiclecommand::CabinCommand::MusicPlay:     return "正在为您播放音乐";
    case vehiclecommand::CabinCommand::MusicNext:     return "已为您切换下一首";
    case vehiclecommand::CabinCommand::MusicPrevious: return "已为您切换上一首";
    case vehiclecommand::CabinCommand::MusicPause:    return "已暂停音乐播放";
    case vehiclecommand::CabinCommand::MusicClose:    return "已为您关闭音乐";
    case vehiclecommand::CabinCommand::MapShow:       return "已为您打开地图";
    case vehiclecommand::CabinCommand::MapZoomIn:     return "已为您放大地图";
    case vehiclecommand::CabinCommand::MapZoomOut:    return "已为您缩小地图";
    case vehiclecommand::CabinCommand::MonitorShow:   return "已为您打开监控";
    case vehiclecommand::CabinCommand::WeatherShow:   return "已为您打开天气";
    case vehiclecommand::CabinCommand::Unknown:       return "";
    }
    return "";
}

void MainWindow::executeCabinCommand(vehiclecommand::CabinCommand command)
{
    switch (command) {
    case vehiclecommand::CabinCommand::MusicShow:
        emit SendCommandToMusic(MUSIC_COMMAND_SHOW);
        break;
    case vehiclecommand::CabinCommand::MusicPlay:
        emit SendCommandToMusic(MUSIC_COMMAND_SHOW);
        emit SendCommandToMusic(MUSIC_COMMAND_PLAY);
        break;
    case vehiclecommand::CabinCommand::MusicNext:
        emit SendCommandToMusic(MUSIC_COMMAND_SHOW);
        emit SendCommandToMusic(MUSIC_COMMAND_NEXT);
        break;
    case vehiclecommand::CabinCommand::MusicPrevious:
        emit SendCommandToMusic(MUSIC_COMMAND_SHOW);
        emit SendCommandToMusic(MUSIC_COMMAND_PRE);
        break;
    case vehiclecommand::CabinCommand::MusicPause:
        emit SendCommandToMusic(MUSIC_COMMAND_SHOW);
        emit SendCommandToMusic(MUSIC_COMMAND_PAUSE);
        break;
    case vehiclecommand::CabinCommand::MusicClose:
        emit SendCommandToMusic(MUSIC_COMMAND_CLOSE);
        this->show();
        break;
    case vehiclecommand::CabinCommand::MapShow:
        emit SendCommandToMap(MAP_COMMAND_SHOW);
        break;
    case vehiclecommand::CabinCommand::MapZoomIn:
        emit SendCommandToMap(MAP_COMMAND_SHOW);
        emit SendCommandToMap(MAP_COMMAND_AMPLIFY);
        break;
    case vehiclecommand::CabinCommand::MapZoomOut:
        emit SendCommandToMap(MAP_COMMAND_SHOW);
        emit SendCommandToMap(MAP_COMMAND_SHRINK);
        break;
    case vehiclecommand::CabinCommand::MonitorShow:
        emit SendCommandToMonitor(MONITOR_COMMAND_SHOW);
        break;
    case vehiclecommand::CabinCommand::WeatherShow:
        weather->show();
        break;
    case vehiclecommand::CabinCommand::Unknown:
        // Handled by dispatchVoiceCommand; nothing to execute.
        break;
    }

    // After the GUI action, send a short TTS feedback announcement when
    // direct TTS feedback is enabled. This is non-blocking: VoiceManager
    // dispatches the ZMQ request on a worker thread, so the GUI thread
    // (and thus input event processing) is never stalled.
    if (voiceManager && voiceManager->isTtsFeedbackEnabled()) {
        const char *feedback = feedbackTextForCommand(command);
        if (feedback[0] != '\0') {
            voiceManager->sendToTts(feedback);
        }
    }
}

bool MainWindow::setBuzzerAlarm(bool active)
{
    if (!buzzer) {
        return false;
    }
    std::string errorMessage;
    if (!buzzer->setAlarm(active, &errorMessage)) {
        VLOG_WARN(MODULE, "setBuzzerAlarm(%d) failed: %s",
                  static_cast<int>(active), errorMessage.c_str());
        return false;
    }
    return true;
}

void MainWindow::onDangerStateChanged(bool active)
{
    updateAlarmSource(vehiclealarm::AlarmSource::DangerObject, active);
}

void MainWindow::updateAlarmSource(vehiclealarm::AlarmSource source,
                                   bool active)
{
    if (!alarmManager.setSourceActive(source, active)) {
        return;
    }

    if (alarmManager.isAlarmActive()) {
        startAlarmPattern();
    } else {
        stopAlarmPattern();
    }
}

void MainWindow::startAlarmPattern()
{
    if (!alarmCycleTimer || !alarmPulseOffTimer) {
        return;
    }

    alarmCycleTimer->stop();
    alarmPulseOffTimer->stop();
    alarmCycleTimer->start();
    onAlarmCycle();
}

void MainWindow::stopAlarmPattern()
{
    if (alarmCycleTimer) {
        alarmCycleTimer->stop();
    }
    if (alarmPulseOffTimer) {
        alarmPulseOffTimer->stop();
    }
    setBuzzerAlarm(false);
}

void MainWindow::onAlarmCycle()
{
    if (!alarmManager.isAlarmActive() || !alarmPulseOffTimer) {
        return;
    }

    if (!setBuzzerAlarm(true)) {
        if (alarmCycleTimer) {
            alarmCycleTimer->stop();
        }
        alarmPulseOffTimer->stop();
        return;
    }
    alarmPulseOffTimer->start();
}

void MainWindow::onAlarmPulseOff()
{
    if (!setBuzzerAlarm(false) && alarmCycleTimer) {
        alarmCycleTimer->stop();
    }
}

void MainWindow::onOfflineAsrTextRecognized(const QString &text)
{
    // Dual-path routing (section 六).
    // The decision logic lives in voice::decideVoiceRoute() so it can be
    // unit-tested without Qt. This slot mirrors the helper exactly.

    // direct_command_enabled=false disables the fixed-command → GUI path.
    // When disabled, all text is treated as unmatched (routed to LLM or logged).
    const bool commandMatched = voiceManager->isDirectCommandEnabled()
        && dispatchVoiceCommand(text);
    const voice::VoiceRoute route = voice::decideVoiceRoute(
        commandMatched,
        voiceManager->shouldFallbackUnknownCommandToLlm(),
        voiceManager->isLlmAvailable());

    switch (route) {
    case voice::VoiceRoute::DirectCommand:
        // Fixed command already executed by dispatchVoiceCommand above.
        // NEVER fall through to LLM (section 六 invariant).
        return;
    case voice::VoiceRoute::SendToLlm: {
        VLOG_INFO(MODULE, "unmatched command, forwarding to LLM: %s",
                  text.toUtf8().constData());
        // sendToLlm posts work to LLM ZMQ client thread; never blocks GUI.
        voiceManager->sendToLlm(text.toUtf8().toStdString());
        return;
    }
    case voice::VoiceRoute::LogUnavailable:
        VLOG_WARN(MODULE,
                  "voice command unmatched and LLM unavailable: text=\"%s\"",
                  text.toUtf8().constData());
        return;
    }
}

void MainWindow::onKeywordDetected(const QString &keyword)
{
    // Wake-word hit: KWS has detected the configured trigger phrase and
    // VoiceManager is now opening the short ASR recognition window.
    // This slot runs on the GUI main thread (AutoConnection queues the
    // signal emitted from VoiceManager's ASR worker thread).
    VLOG_INFO(MODULE, "KWS wake-word hit: %s",
              keyword.toUtf8().constData());

    // Optional short TTS acknowledgement so the user knows the system is
    // now listening for a command. Reuses the same sendToTts() path as
    // fixed-command feedback; non-blocking (ZMQ request is dispatched on
    // a worker thread). Only fires when TTS feedback is enabled in config.
    if (voiceManager && voiceManager->isTtsFeedbackEnabled()) {
        voiceManager->sendToTts("我在");
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *ev)
{
    // Only handle mouse button events; pass everything else through.
    // Casting a non-mouse event to QMouseEvent is undefined behavior.
    if (ev->type() != QEvent::MouseButtonPress &&
        ev->type() != QEvent::MouseButtonRelease) {
        return QWidget::eventFilter(watched, ev);
    }

    QMouseEvent* event = static_cast<QMouseEvent*>(ev);
    int index = ui->stackedWidget->currentIndex();
    int sum = ui->stackedWidget->count();
    // event->type() is already filtered to MouseButtonPress/Release above,
    // so the switch below only handles those two cases explicitly. Other
    // QEvent::Type enum values are unreachable here. -Wswitch-enum (enabled
    // globally in CarTerminal.pro for CabinCommand::toString() defense) would
    // otherwise force listing ~100 irrelevant enum values, so suppress it
    // locally for this defensive switch only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
        switch (event->type()) {
        case QEvent::MouseButtonPress:
            VLOG_DEBUG(MODULE, "mouse press at (%d, %d)",
                       event->globalX(), event->globalY());
            m_pressPoint.setY(event->globalY());     // 记录按下点的y坐标
            m_pressPoint.setX(event->globalX());     // 记录按下点的x坐标
            break;
        case QEvent::MouseButtonRelease:
            VLOG_DEBUG(MODULE, "mouse release at (%d, %d)",
                       event->globalX(), event->globalY());
            m_releasePoint.setY(event->globalY());     // 记录按下点的y坐标
            m_releasePoint.setX(event->globalX());     // 记录按下点的x坐标

            if(m_releasePoint.x()-m_pressPoint.x()>200)
            {
                ui->stackedWidget->setCurrentIndex(index<(sum-1)?index+1:0);
            }
            else if(m_pressPoint.x()-m_releasePoint.x()>200)
            {
                ui->stackedWidget->setCurrentIndex(index>0?index-1:(sum-1));
            }
            break;
        default:
            break;
        }
#pragma GCC diagnostic pop
    return QWidget::eventFilter(watched,event);//将事件传递给父类
}

void MainWindow::on_pBtn_Music_clicked()
{
    musicPlayer->show();
}

void MainWindow::on_pBtn_Weather_clicked()
{
    weather->show();
}

void MainWindow::on_pBtn_Monitor_clicked()
{
    monitor->show();
    monitor->myStart();
}

void MainWindow::on_pBtn_Map_clicked()
{
    baiduMap->show();
}

void MainWindow::updateTimeDisplay()
{
    QString time = QTime::currentTime().toString("hh:mm:ss");
    QString date = QDate::currentDate().toString("yyyy-MM-dd");
    ui->label_Time->setText(time);
    ui->label_Date->setText(date);
}

void MainWindow::on_handleRecord()
{
    VLOG_INFO(MODULE, "on_handleRecord: starting ASR request");
    if (!asrConfig.asrEnabled) {
        // Defense in depth: online ASR is disabled by config. Do not post any
        // credential-bearing request to the ASR backend.
        VLOG_WARN(MODULE, "on_handleRecord: online ASR disabled; request skipped");
        return;
    }
    QFile file(QString::fromStdString(asrConfig.recordPath));
    if (!file.exists()) {
        VLOG_ERROR(MODULE, "record file not found: %s",
                   asrConfig.recordPath.c_str());
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        VLOG_ERROR(MODULE, "cannot open record file: %s",
                   asrConfig.recordPath.c_str());
        return;
    }
    request->setUrl(QUrl::fromUserInput(
        QString::fromStdString(asrConfig.apiUrl)));
    QByteArray fileData = file.readAll();
    file.close();
    QByteArray base64Encoded = fileData.toBase64();
    QJsonObject obj;
    obj.insert("format", QJsonValue(QString::fromStdString(asrConfig.format)));
    obj.insert("rate", QJsonValue(asrConfig.rate));
    obj.insert("channel", QJsonValue(asrConfig.channel));
    obj.insert("cuid", QJsonValue(QString::fromStdString(asrConfig.cuid)));
    obj.insert("token", QJsonValue(QString::fromStdString(asrConfig.token)));
    obj.insert("speech", QJsonValue(QString(base64Encoded)));
    obj.insert("len", QJsonValue(fileData.length()));
    QByteArray byteArray = QJsonDocument(obj).toJson();
    networkManage->post(*request, byteArray);
    VLOG_DEBUG(MODULE, "ASR request sent (%d bytes audio)",
               fileData.length());
}

void MainWindow::loadAllConfigs()
{
    // Load device config
    std::string deviceConfigPath = getEnvOr("VEHICLE_DEVICE_CONFIG",
                                            "config/vehicle_devices.conf");
    bool deviceConfigLoaded = false;
    std::string deviceConfigError;
    deviceConfig = vehicledevice::loadDeviceConfigFile(deviceConfigPath,
                                                       &deviceConfigLoaded,
                                                       &deviceConfigError);
    if (!deviceConfigLoaded) {
        VLOG_WARN(MODULE, "device config: %s", deviceConfigError.c_str());
    }

    // Load Week10 Front camera config. VEHICLE_CAMERA_CONFIG remains the
    // backward-compatible override; the role-named default avoids rewriting
    // the legacy single-camera config while CAM3 is being revalidated.
    std::string cameraConfigPath = getEnvOr("VEHICLE_CAMERA_CONFIG",
                                            "config/front_camera_settings.conf");
    bool cameraConfigLoaded = false;
    std::string cameraConfigError;
    cameraConfig = vehiclecamera::loadCameraConfigFile(
        cameraConfigPath, &cameraConfigLoaded, &cameraConfigError);
    if (!cameraConfigLoaded) {
        VLOG_WARN(MODULE, "front camera config: %s",
                  cameraConfigError.c_str());
    }

    // Rear has an independent config and lifecycle. Reuse the proven parser
    // instead of adding a second key namespace to CameraConfig.
    std::string rearCameraConfigPath = getEnvOr(
        "VEHICLE_REAR_CAMERA_CONFIG",
        "config/rear_camera_settings.conf");
    bool rearCameraConfigLoaded = false;
    std::string rearCameraConfigError;
    rearCameraConfig = vehiclecamera::loadCameraConfigFile(
        rearCameraConfigPath,
        &rearCameraConfigLoaded,
        &rearCameraConfigError);
    if (!rearCameraConfigLoaded) {
        VLOG_WARN(MODULE, "rear camera config: %s",
                  rearCameraConfigError.c_str());
    }

    // Load ASR config
    std::string asrConfigPath = getEnvOr("VEHICLE_ASR_CONFIG",
                                         "config/asr_settings.conf");
    loadAsrConfig(asrConfigPath);

    // Load RKNN detector config
    std::string rknnConfigPath = getEnvOr("VEHICLE_RKNN_CONFIG",
                                          "config/rknn_settings.conf");
    bool rknnConfigLoaded = false;
    std::string rknnConfigError;
    rknnConfig = rknn::loadDetectorConfigFile(rknnConfigPath,
                                              &rknnConfigLoaded,
                                              &rknnConfigError);
    if (!rknnConfigLoaded) {
        VLOG_WARN(MODULE, "RKNN config: %s", rknnConfigError.c_str());
    }

    // Load voice interaction config
    std::string voiceConfigPath = getEnvOr("VEHICLE_VOICE_CONFIG",
                                           "config/voice_settings.conf");
    voiceConfig = voice::VoiceManager::loadConfig(voiceConfigPath);

    // Load RTSP server config
    std::string rtspConfigPath = getEnvOr("VEHICLE_RTSP_CONFIG",
                                          "config/rtsp_settings.conf");
    rtspConfig.enabled = false;
    rtspConfig.port = 8554;
    rtspConfig.streamPath = "/live";
    rtspConfig.width = cameraConfig.width > 0 ? cameraConfig.width : 800;
    rtspConfig.height = cameraConfig.height > 0 ? cameraConfig.height : 600;
    rtspConfig.fps = 25;
    rtspConfig.bitrate = 2000000;
    rtspConfig.gopSize = 25;
    {
        QFile rtspFile(QString::fromStdString(rtspConfigPath));
        if (rtspFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!rtspFile.atEnd()) {
                QString line = QString::fromUtf8(rtspFile.readLine()).trimmed();
                if (line.isEmpty() || line.startsWith('#')) {
                    continue;
                }
                QStringList parts = line.split('=');
                if (parts.size() < 2) {
                    continue;
                }
                QString key = parts[0].trimmed();
                QString value = parts.mid(1).join('=').trimmed();
                if (key == "rtsp_enabled") {
                    rtspConfig.enabled = (value == "true");
                } else if (key == "rtsp_port") {
                    rtspConfig.port = value.toInt();
                } else if (key == "rtsp_stream_path") {
                    rtspConfig.streamPath = value.toStdString();
                } else if (key == "rtsp_width") {
                    rtspConfig.width = value.toInt();
                } else if (key == "rtsp_height") {
                    rtspConfig.height = value.toInt();
                } else if (key == "rtsp_fps") {
                    rtspConfig.fps = value.toInt();
                } else if (key == "rtsp_bitrate") {
                    rtspConfig.bitrate = value.toInt();
                } else if (key == "rtsp_gop_size") {
                    rtspConfig.gopSize = value.toInt();
                } else if (key == "rtsp_codec") {
                    rtspConfig.codec = value.toStdString();
                }
            }
            rtspFile.close();
            VLOG_INFO(MODULE, "RTSP config loaded: enabled=%d port=%d",
                      rtspConfig.enabled, rtspConfig.port);
        } else {
            VLOG_WARN(MODULE, "RTSP config not found: %s, using defaults",
                      rtspConfigPath.c_str());
        }
    }

    // Load MP4 recording sub-config (consumes Annex-B from RtspServer encoder).
    // Stored as rtspConfig.recording; codec/width/height/fps are inherited
    // from the RTSP config at RtspServer::init() time, so this file only
    // carries output_dir / segment_duration / max_segments / enabled.
    {
        std::string recordingConfigPath = getEnvOr("VEHICLE_RECORDING_CONFIG",
                                                   "config/recording_settings.conf");
        rtspConfig.recording.enabled = false;
        rtspConfig.recording.outputDir = "recordings";
        rtspConfig.recording.filenamePrefix = "record";
        rtspConfig.recording.segmentDurationSec = 60;
        rtspConfig.recording.maxSegments = 10;
        // width/height/fps/codec are deliberately left at defaults — they
        // are overwritten by RtspServer::init() to match the RTSP stream.

        QFile recFile(QString::fromStdString(recordingConfigPath));
        if (recFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!recFile.atEnd()) {
                QString line = QString::fromUtf8(recFile.readLine()).trimmed();
                if (line.isEmpty() || line.startsWith('#')) {
                    continue;
                }
                QStringList parts = line.split('=');
                if (parts.size() < 2) {
                    continue;
                }
                QString key = parts[0].trimmed();
                QString value = parts.mid(1).join('=').trimmed();
                if (key == "recording_enabled") {
                    rtspConfig.recording.enabled = (value == "true");
                } else if (key == "recording_output_dir") {
                    rtspConfig.recording.outputDir = value.toStdString();
                } else if (key == "recording_filename_prefix") {
                    rtspConfig.recording.filenamePrefix = value.toStdString();
                } else if (key == "recording_segment_duration_sec") {
                    rtspConfig.recording.segmentDurationSec = value.toInt();
                } else if (key == "recording_max_segments") {
                    rtspConfig.recording.maxSegments = value.toInt();
                }
            }
            recFile.close();
            VLOG_INFO(MODULE,
                      "recording config loaded: enabled=%d dir=%s seg=%ds max=%d",
                      rtspConfig.recording.enabled,
                      rtspConfig.recording.outputDir.c_str(),
                      rtspConfig.recording.segmentDurationSec,
                      rtspConfig.recording.maxSegments);
        } else {
            VLOG_WARN(MODULE, "recording config not found: %s, using defaults",
                      recordingConfigPath.c_str());
        }
    }
}

void MainWindow::loadAsrConfig(const std::string &configPath)
{
    // Safe defaults: online ASR stays disabled until a real config enables it.
    // No real cuid/token is hardcoded here; placeholders are treated as invalid.
    asrConfig.asrEnabled = false;
    asrConfig.apiUrl = "http://vop.baidu.com/server_api";
    asrConfig.token = "";
    asrConfig.cuid = "";
    asrConfig.format = "pcm";
    asrConfig.rate = 16000;
    asrConfig.channel = 1;
    asrConfig.recordPath = "./record.wav";
    asrConfig.timeoutMs = 5000;

    QFile configFile(QString::fromStdString(configPath));
    if (!configFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        VLOG_WARN(MODULE, "ASR config not found: %s, online ASR disabled",
                  configPath.c_str());
        return;
    }

    while (!configFile.atEnd()) {
        QString line = QString::fromUtf8(configFile.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }
        QStringList parts = line.split('=');
        if (parts.size() < 2) {
            continue;
        }
        QString key = parts[0].trimmed();
        QString value = parts.mid(1).join('=').trimmed();
        if (key == "asr_enabled") {
            const QString v = value.toLower();
            asrConfig.asrEnabled = (v == "true" || v == "1" ||
                                    v == "yes" || v == "on");
        } else if (key == "asr_api_url") {
            asrConfig.apiUrl = value.toStdString();
        } else if (key == "asr_token") {
            asrConfig.token = value.toStdString();
        } else if (key == "asr_cuid") {
            asrConfig.cuid = value.toStdString();
        } else if (key == "asr_format") {
            asrConfig.format = value.toStdString();
        } else if (key == "asr_rate") {
            asrConfig.rate = value.toInt();
        } else if (key == "asr_channel") {
            asrConfig.channel = value.toInt();
        } else if (key == "asr_record_path") {
            asrConfig.recordPath = value.toStdString();
        } else if (key == "asr_timeout_ms") {
            asrConfig.timeoutMs = value.toInt();
        }
    }
    configFile.close();

    // Placeholder credentials committed in example/template configs must
    // never be treated as a valid online ASR configuration.
    const bool cuidIsPlaceholder =
        asrConfig.cuid == "YOUR_ASR_CUID" || asrConfig.cuid.empty();
    const bool tokenIsPlaceholder =
        asrConfig.token == "YOUR_ASR_TOKEN" || asrConfig.token.empty();
    if (asrConfig.asrEnabled && (cuidIsPlaceholder || tokenIsPlaceholder)) {
        VLOG_WARN(MODULE,
                  "ASR enabled but cuid/token missing or placeholder; "
                  "disabling online ASR");
        asrConfig.asrEnabled = false;
    }

    if (!asrConfig.asrEnabled) {
        VLOG_INFO(MODULE, "online ASR disabled (config asr_enabled=false or "
                          "missing/placeholder credentials)");
    } else {
        VLOG_INFO(MODULE, "ASR config loaded: url=%s record=%s",
                  asrConfig.apiUrl.c_str(), asrConfig.recordPath.c_str());
    }
}
