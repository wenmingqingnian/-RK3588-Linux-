#ifndef MONITOR_H
#define MONITOR_H

#include <QMainWindow>
#include <QLabel>
#include <QImage>
#include <QTimer>
#include <QPushButton>
#include <QHBoxLayout>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>

#include "capture_thread.h"
#include "ap3216.h"
#include "vehicledeviceconfig.h"
#include "cameraconfig.h"
#include "camera_router.h"
#include "alarmpolicy.h"
#include "rknn_detector.h"
#include "rtsp_server.h"
#include "media_pipeline.h"
#include "frame_packet.h"
#define MONITOR_COMMAND_CLOSE 0
#define MONITOR_COMMAND_SHOW 1
namespace Ui {
class Monitor;
}

class Monitor : public QMainWindow
{
    Q_OBJECT
public:
    static Monitor* getInstance(
        const vehicledevice::Ap3216Config &ap3216Config =
            vehicledevice::Ap3216Config(),
        const vehiclecamera::CameraConfig &cameraConfig =
            vehiclecamera::CameraConfig(),
        const vehiclecamera::CameraConfig &rearCameraConfig =
            vehiclecamera::CameraConfig(),
        const rknn::DetectorConfig &rknnConfig =
            rknn::DetectorConfig(),
        const media::RtspServerConfig &rtspConfig =
            media::RtspServerConfig());

    // Destroy the manually owned singleton and release its resources.
    static void destroyInstance();
signals:
    void dangerStateChanged(bool active);
    // Emitted by the pipeline's GUI consumer thread (queued connection) to
    // deliver an RGB32 frame for display. The worker thread must not touch
    // QWidget directly.
    void pipelineFrameReady(QImage frame);
private:
    explicit Monitor(const vehicledevice::Ap3216Config &ap3216Config,
                     const vehiclecamera::CameraConfig &cameraConfig,
                     const vehiclecamera::CameraConfig &rearCameraConfig,
                     const rknn::DetectorConfig &rknnConfig,
                     const media::RtspServerConfig &rtspConfig,
                     QWidget *parent = nullptr);
    static Monitor* monitor;
    ~Monitor();

private:
    Ui::Monitor *ui;
    void restart();
    /* 用于显示捕获到的图像 */
    QLabel *videoLabel;

    /* Week10 heterogeneous cameras: Front=IMX415/CAM3, Rear=IMX335/CAM1. */
    CaptureThread *m_captureModule = nullptr;       // Front
    CaptureThread *m_rearCaptureModule = nullptr;  // Rear
    vehiclecamera::CameraRouter *m_cameraRouter = nullptr;
    vehiclecamera::CameraConfig m_rearCameraConfig;
    QLabel *cameraRoleLabel = nullptr;
    bool m_rearFrameReceived = false;
    void startV4l2Cameras();
    void stopV4l2Cameras();
    void updateCameraRoleLabel();
    void handleRearCaptureRequest(bool active);

    /* 开始捕获图像按钮 */
    QPushButton *startCaptureButton = nullptr;

    QLabel *ap3216StatusLabel = nullptr;
    QLabel *ap3216AlertLabel = nullptr;
    bool ap3216Available = false;
    bool ap3216AlertActive = false;
    QString ap3216Backend = "--";
    QString ap3216Als = "--";
    QString ap3216Ps = "--";
    QString ap3216Ir = "--";
    QString ap3216AlertSource = "--";
    void refreshAp3216Panel();

    /*      Ap3216c   */
    Ap3216c *ap3216 = nullptr;

    /* RKNN目标检测 */
    rknn::DetectorConfig m_rknnConfig;
    bool m_rknnReady;
    vehiclealarm::DangerAlarmPolicy m_dangerAlarmPolicy;
    // RKNN detection runs on a dedicated worker thread (~80ms per inference).
    // The GUI thread feeds frames via a drop-olatest single slot and draws
    // the last detection result (may lag 1-3 frames, acceptable for alerts).
    // This avoids blocking the GUI event loop and prevents Qt event-queue
    // buildup that previously caused cumulative display latency.
    int m_rknnFrameSkip = 0;  // retained for compatibility, unused in async path
    std::thread m_rknnThread;
    std::atomic<bool> m_rknnThreadRunning{false};
    std::mutex m_rknnMutex;
    std::condition_variable m_rknnCv;
    QImage m_rknnPendingFrame;
    bool m_rknnHasFrame{false};
    std::vector<rknn::Detection> m_lastDetections;
    void rknnWorkerLoop();

    /* RTSP推流服务器 */
    media::RtspServer *m_rtspServer;

    /* 媒体管线（test_pattern/file 源使用，3支路独立队列） */
    media::MediaPipeline *m_pipeline;
    QTimer *m_pipelineStatsTimer;
    bool m_usePipeline;  // true=test_pattern/file, false=v4l2(CaptureThread)
    vehiclecamera::CameraConfig m_cameraConfig;  // stored for pipeline source creation
    std::vector<uint8_t> m_guiRgbBuffer;   // GUI consumer scratch buffer
    std::vector<uint8_t> m_rknnRgbBuffer;  // RKNN consumer scratch buffer
    std::vector<uint8_t> m_rtspRgbBuffer;  // RTSP consumer scratch buffer
    // GUI frame throttling: at most one pending pipelineFrameReady event in
    // the Qt event queue. The worker thread drops newer frames while one is
    // still queued so mouse/key events are not starved at 30 fps.
    // Uses std::atomic instead of QAtomicInt because Qt 5.15 QAtomicInt
    // lacks the fetchAdd/fetchSub members (Qt 6 only).
    std::atomic<int> m_pendingGuiFrames{0};
    void startPipelineSource();
    void stopPipelineSource();

    /* 测试图案生成器（摄像头未连接时使用） */
    QTimer *m_testFrameTimer;
    QTimer *m_frameWatchdog;
    bool m_cameraFrameReceived;
    int m_testFrameIndex;
    void startTestFrameGenerator();

    /* 重写大小事件 */
    void resizeEvent(QResizeEvent *event) override;
    /* Esc/Back 键退出 Monitor（备用路径，防止按钮被视频层遮挡时无法返回） */
    void keyPressEvent(QKeyEvent *event) override;
public :
    void myStart();
    void myStop();
public slots:
    // Front always feeds RKNN/RTSP; CameraRouter decides local display role.
    void showImage(QImage);
    // Rear is GUI-only in W10-P1. Rear RKNN remains an optional later task.
    void showRearImage(QImage);
    void setReverseActive(bool active);
    /* 开始采集按钮被点击 */
    void startCaptureButtonClicked(bool);
    void on_ap3216_data_updated(QString als, QString ps, QString ir);
    void on_ap3216_availability_changed(bool available);
    void on_ap3216_backend_changed(QString backend);
    void on_ap3216_alert_changed(bool active, QString source);
    void on_handleCommand(int);
    void onTestFrameTick();
    void onFrameWatchdog();
    void onPipelineStats();
    /* 接收 MediaPipeline GUI 消费者线程通过 queued signal 传递的帧 */
    void onPipelineFrame(QImage frame);
};


#endif // MONITOR_H
