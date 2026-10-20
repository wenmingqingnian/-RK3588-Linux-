#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QVBoxLayout>
#include <QParallelAnimationGroup>
#include "settingwindow.h"
#include "clock.h"
#include "dht11.h"
#include "mpu6050.h"
#include "vehicledeviceconfig.h"
#include "buzzerdevice.h"
#include "cameraconfig.h"
#include "rknn_detector.h"
#include "rtsp_server.h"
#include "vehiclecommand.h"
#include "alarmmanager.h"
#include "alarmpolicy.h"
#include "Map/baidumap.h"
#include "Music/musicplayer.h"
#include "Weather/weather.h"
#include "Monitor/monitor.h"
#include "speechrecognition.h"
#include "VoiceManager.h"
#include <QTimer>
#include <QPoint>
QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }

class VehicleIoInput;
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    // Set the buzzer alarm on/off. Returns true on success.
    // Safe to call when buzzer is disabled in config (returns false).
    bool setBuzzerAlarm(bool active);
signals:
    void SendCommandToMusic(int);
    void SendCommandToMap(int);
    void SendCommandToMonitor(int);
private slots:
    void on_pBtn_Setting_clicked();
    void updateHumidityTemp(QString, QString);
    void getSpeechResult(QNetworkReply *reply);

    void on_pBtn_Music_clicked();

    void on_pBtn_Weather_clicked();

    void on_pBtn_Monitor_clicked();

    void on_pBtn_Map_clicked();
    void updateTimeDisplay();

    void on_handleRecord();

    // Offline ASR text -> dual-path routing (section 五/六).
    // Runs on the GUI main thread via Qt::AutoConnection (the signal is
    // emitted from VoiceManager's ASR worker thread, so the connection
    // is automatically queued).
    void onOfflineAsrTextRecognized(const QString &text);

    // KWS wake-word hit handler.
    // VoiceManager emits keywordDetected() right before opening the ASR
    // short window. This slot logs the hit and (when TTS feedback is
    // enabled) plays a short "我在" announcement so the user knows the
    // system is now listening for a command. Runs on the GUI main thread.
    void onKeywordDetected(const QString &keyword);
    void onDangerStateChanged(bool active);
    void onAlarmCycle();
    void onAlarmPulseOff();

private:
    void loadAsrConfig(const std::string &configPath);

    // Load all module configs (device/camera/rknn/voice/rtsp) from files
    // pointed to by VEHICLE_*_CONFIG env vars. Extracted from ctor for
    // readability; called once during construction.
    void loadAllConfigs();

    // Shared command dispatch for both legacy online ASR and offline ASR.
    // Returns true when a fixed command matched and was executed.
    // Returns false for Unknown; caller decides LLM fallback.
    bool dispatchVoiceCommand(const QString &text);

    // Execute the GUI action for a parsed cabin command. Pure GUI work,
    // must run on the main thread. No keyword parsing here.
    void executeCabinCommand(vehiclecommand::CabinCommand command);
    void updateAlarmSource(vehiclealarm::AlarmSource source, bool active);
    void startAlarmPattern();
    void stopAlarmPattern();

    struct AsrConfig {
        bool asrEnabled = false;
        std::string apiUrl;
        std::string token;
        std::string cuid;
        std::string format;
        int rate = 16000;
        int channel = 1;
        std::string recordPath;
        int timeoutMs = 5000;
    };

    Ui::MainWindow *ui;
    Clock clock;
    SettingWindow settingWindow;
    bool eventFilter(QObject *watched, QEvent *event);
    QParallelAnimationGroup *myAnimationGroup;
    Dht11Reader *dht11Reader;
    Mpu6050Reader *mpu6050Reader;
    vehicledevice::VehicleDeviceConfig deviceConfig;
    // Week10 role mapping: cameraConfig=Front, rearCameraConfig=Rear.
    vehiclecamera::CameraConfig cameraConfig;
    vehiclecamera::CameraConfig rearCameraConfig;
    rknn::DetectorConfig rknnConfig;
    media::RtspServerConfig rtspConfig;
    voice::VoiceConfig voiceConfig;
    AsrConfig asrConfig;

    // QObject interface
    Monitor *monitor;
    Weather *weather;
    MusicPlayer *musicPlayer;
    BaiduMap *baiduMap;
    QTimer *time;
    SpeechRecognition *AsrThread;
    voice::VoiceManager *voiceManager;
    QNetworkAccessManager *networkManage;
    QNetworkRequest *request;
    vehicledevice::BuzzerDevice *buzzer;
    VehicleIoInput *vehicleIoInput = nullptr;
    vehiclealarm::AlarmManager alarmManager;
    vehiclealarm::TemperatureAlarmPolicy temperatureAlarmPolicy;
    QTimer *alarmCycleTimer = nullptr;
    QTimer *alarmPulseOffTimer = nullptr;

    // Mouse press/release positions for swipe gesture detection in eventFilter.
    // Replaces previous file-scope globals (point/last_point) to avoid
    // shared state across MainWindow instances and improve encapsulation.
    QPoint m_pressPoint;
    QPoint m_releasePoint;
    QVector<QMainWindow *> windows;

    // Tracks whether the legacy online ASR (SpeechRecognition) was actually
    // started. When offline ASR is ready we must NOT start the legacy path
    // to avoid both modules holding the microphone at the same time.
    bool legacyAsrStarted = false;

};
#endif // MAINWINDOW_H
