#include "monitor.h"
#include "ui_monitor.h"
#include "ap3216.h"
#include "logmanager.h"
#include "rga_converter.h"
#include "test_pattern_source.h"
#include "file_frame_source.h"

#include <QPainter>
#include <QMetaObject>
#include <QKeyEvent>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <cmath>

static const char *MODULE = "monitor";

Monitor * Monitor::monitor = nullptr;

Monitor * Monitor::getInstance(const vehicledevice::Ap3216Config &ap3216Config,
                               const vehiclecamera::CameraConfig &cameraConfig,
                               const vehiclecamera::CameraConfig &rearCameraConfig,
                               const rknn::DetectorConfig &rknnConfig,
                               const media::RtspServerConfig &rtspConfig)
{
    if (monitor == nullptr) {
        monitor = new Monitor(ap3216Config, cameraConfig, rearCameraConfig,
                              rknnConfig, rtspConfig);
    }
    return monitor;
}


void Monitor::destroyInstance()
{
    // Clear the global pointer before deletion so re-entrant code cannot
    // observe a singleton whose destruction is already in progress.
    Monitor *instance = monitor;
    monitor = nullptr;
    delete instance;
}

Monitor::Monitor(const vehicledevice::Ap3216Config &ap3216Config,
                 const vehiclecamera::CameraConfig &cameraConfig,
                 const vehiclecamera::CameraConfig &rearCameraConfig,
                 const rknn::DetectorConfig &rknnConfig,
                 const media::RtspServerConfig &rtspConfig,
                 QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::Monitor),
    m_rtspServer(nullptr),
    m_pipeline(nullptr),
    m_pipelineStatsTimer(nullptr),
    m_usePipeline(false),
    m_testFrameTimer(nullptr),
    m_frameWatchdog(nullptr),
    m_cameraFrameReceived(false),
    m_testFrameIndex(0)
{
    this->setGeometry(0, 0, 1024, 600);

    ap3216 = new Ap3216c(ap3216Config, this);

    videoLabel = new QLabel(this);
    videoLabel->setText("未获取到图像数据");
    videoLabel->setStyleSheet("QWidget {color: white;}");
    videoLabel->setAlignment(Qt::AlignCenter);
    videoLabel->resize(1024, 600);
    // Keep the video label at the bottom of the Z-order so the controls and
    // AP3216C status panel remain visible and clickable above live video.
    videoLabel->lower();

    ap3216Backend = QString::fromStdString(ap3216Config.backend);
    ap3216StatusLabel = new QLabel(this);
    ap3216StatusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ap3216StatusLabel->setStyleSheet(
        "QLabel { color: white; background-color: rgba(0,0,0,170); "
        "border: 1px solid rgba(255,255,255,120); border-radius: 6px; "
        "padding: 8px; }");
    QFont sensorFont("Sans Serif", 11);
    sensorFont.setBold(true);
    ap3216StatusLabel->setFont(sensorFont);

    ap3216AlertLabel = new QLabel(this);
    ap3216AlertLabel->setAlignment(Qt::AlignCenter);
    ap3216AlertLabel->setFont(sensorFont);
    refreshAp3216Panel();
    /* 按钮 */
    startCaptureButton = new QPushButton(this);
    startCaptureButton->setCheckable(true);
    startCaptureButton->setText("返回");

    /* 设置背景颜色为黑色 */
    QColor color = QColor(Qt::black);
    QPalette p;
    p.setColor(QPalette::Window, color);
    this->setPalette(p);

    /* 样式表 */
    startCaptureButton->setStyleSheet("QPushButton {background-color: white; border-radius: 30}"
                                      "QPushButton:pressed  {background-color: red;}");

    // Explicitly position the "返回" button and distance label at the
    // bottom-center of the 1024x600 window, and raise them above the video
    // label so they stay clickable when video frames are flowing at 30 fps.
    // Previously the position was set only in resizeEvent(), which left the
    // button at (0,0) with default size until the first resize — making it
    // appear missing or unclickable.
    startCaptureButton->resize(200, 40);
    startCaptureButton->move((this->width() - 200) / 2, this->height() - 80);
    ap3216StatusLabel->resize(300, 150);
    ap3216StatusLabel->move(20, 20);
    ap3216AlertLabel->resize(240, 52);
    ap3216AlertLabel->move(this->width() - 260, 20);
    startCaptureButton->raise();
    ap3216StatusLabel->raise();
    ap3216AlertLabel->raise();

    m_cameraRouter = new vehiclecamera::CameraRouter(this);
    m_cameraConfig = cameraConfig;          // Front / pipeline source config
    m_rearCameraConfig = rearCameraConfig;  // Rear V4L2 config

    cameraRoleLabel = new QLabel(this);
    cameraRoleLabel->setAlignment(Qt::AlignCenter);
    cameraRoleLabel->setStyleSheet(
        "QLabel { color: white; background-color: rgba(0,0,0,170); "
        "border: 1px solid rgba(255,255,255,120); border-radius: 6px; "
        "padding: 6px; }");
    cameraRoleLabel->resize(360, 52);
    cameraRoleLabel->move((this->width() - 360) / 2, 20);
    cameraRoleLabel->raise();

    m_captureModule = new CaptureThread(this);
    if (!m_captureModule->init(cameraConfig)) {
        VLOG_ERROR(MODULE, "Front camera init failed: %s",
                   m_captureModule->lastErrorMessage().c_str());
    }

    m_rearCaptureModule = new CaptureThread(this);
    if (rearCameraConfig.sourceType != "v4l2") {
        VLOG_WARN(MODULE,
                  "Rear source '%s' is not supported by W10-P1 dual-V4L2 path; "
                  "Rear will remain unavailable",
                  rearCameraConfig.sourceType.c_str());
    } else if (!m_rearCaptureModule->init(rearCameraConfig)) {
        VLOG_ERROR(MODULE, "Rear camera init failed: %s",
                   m_rearCaptureModule->lastErrorMessage().c_str());
    }

    connect(m_cameraRouter,
            &vehiclecamera::CameraRouter::displayRoleChanged,
            this,
            [this](vehiclecamera::CameraRole role) {
                VLOG_INFO(MODULE, "GUI camera role -> %s",
                          vehiclecamera::cameraRoleToString(role));
                updateCameraRoleLabel();
            });
    connect(m_cameraRouter,
            &vehiclecamera::CameraRouter::rearFallbackChanged,
            this,
            [this](bool active) {
                VLOG_WARN(MODULE, "Rear fallback -> %s",
                          active ? "Front" : "cleared");
                updateCameraRoleLabel();
            });
    connect(m_cameraRouter,
            &vehiclecamera::CameraRouter::rearCaptureRequested,
            this,
            &Monitor::handleRearCaptureRequest);
    updateCameraRoleLabel();

    // Determine whether to use the new MediaPipeline (test_pattern/file) or
    // the legacy CaptureThread path (v4l2). The pipeline provides three
    // independent bounded queues (GUI/RKNN/RTSP) so slow consumers do not
    // block each other.
    m_usePipeline = (cameraConfig.sourceType == "test_pattern" ||
                     cameraConfig.sourceType == "file");
    if (m_usePipeline) {
        m_pipeline = new media::MediaPipeline(cameraConfig.queueCapacity,
                                              cameraConfig.readTimeoutMs);
        m_pipelineStatsTimer = new QTimer(this);
        m_pipelineStatsTimer->setInterval(5000);
        connect(m_pipelineStatsTimer, SIGNAL(timeout()),
                this, SLOT(onPipelineStats()));
        VLOG_INFO(MODULE, "using MediaPipeline (source=%s)",
                  cameraConfig.sourceType.c_str());
    }

    // Initialize RKNN detector.
    m_rknnConfig = rknnConfig;
    m_rknnReady = false;
    if (rknnConfig.enabled) {
        m_rknnReady = rknn::initDetector(rknnConfig);
        if (!m_rknnReady) {
            VLOG_ERROR(MODULE, "RKNN detector init failed; detection disabled");
        }
    } else {
        VLOG_INFO(MODULE, "RKNN detection disabled");
    }

    // Start the RKNN worker thread (async detection). Moving the ~80ms
    // inference off the GUI thread eliminates preview stutter and event-
    // queue buildup that previously caused cumulative display latency.
    // The pipeline path (test_pattern/file) has its own RKNN consumer
    // thread and does not need this worker.
    if (m_rknnReady && !m_usePipeline) {
        m_rknnThreadRunning = true;
        m_rknnThread = std::thread(&Monitor::rknnWorkerLoop, this);
    }

    // Initialize RTSP streaming server.
    m_rtspServer = new media::RtspServer();
    media::RtspStatus rtspStatus = m_rtspServer->init(rtspConfig);
    if (rtspStatus == media::RtspStatus::Success) {
        rtspStatus = m_rtspServer->start();
    }
    if (rtspStatus == media::RtspStatus::Success) {
        VLOG_INFO(MODULE, "RTSP server started");
    } else {
        VLOG_INFO(MODULE, "RTSP server not started: %s",
                  media::rtspStatusToString(rtspStatus));
    }

    // Auto-start MP4 recording when RTSP is running and recording is enabled
    // in config. Recorder was initialized in RtspServer::init() above; this
    // opens the first segment file so writeFrame() can start accepting data
    // as soon as the encoder produces frames. startRecording() is a no-op
    // (returns false silently) when recording is disabled in config.
    if (m_rtspServer && m_rtspServer->isRunning()) {
        m_rtspServer->startRecording();
    }

    connect(startCaptureButton, SIGNAL(clicked(bool)),
            this, SLOT(startCaptureButtonClicked(bool)));
    // W10-P1A legacy V4L2 GUI single-pending-frame throttle.
    //
    // CaptureThread emits a deep-copied 800x600 RGB32 QImage for every frame.
    // A normal cross-thread AutoConnection can therefore enqueue frames without
    // a bound when the GUI thread is slower than capture, eventually causing OOM.
    //
    // Run this lightweight gate in the capture thread and allow at most one
    // GUI callback to be pending. Newer frames are dropped while that callback
    // is still queued. QTimer::singleShot(context, functor) executes showImage()
    // on the Monitor/GUI thread, so QWidget access remains thread-safe.
    connect(m_captureModule,
            &CaptureThread::imageReady,
            this,
            [this](QImage frame) {
                if (m_pendingGuiFrames.fetch_add(1) >= 1) {
                    m_pendingGuiFrames.fetch_sub(1);
                    return;
                }

                QTimer::singleShot(0, this, [this, frame]() {
                    showImage(frame);
                    m_pendingGuiFrames.fetch_sub(1);
                });
            },
            Qt::DirectConnection);
    connect(m_rearCaptureModule, SIGNAL(imageReady(QImage)),
            this, SLOT(showRearImage(QImage)));
    connect(m_rearCaptureModule, &CaptureThread::frameError,
            this, [this](const QString &description) {
                VLOG_WARN(MODULE, "Rear capture frame error: %s",
                          description.toUtf8().constData());
                if (m_cameraRouter) {
                    m_cameraRouter->setAvailable(
                        vehiclecamera::CameraRole::Rear, false);
                }
            });
    // Pipeline frame delivery uses a queued connection so the worker thread
    // never touches QWidget directly.
    connect(this, SIGNAL(pipelineFrameReady(QImage)),
            this, SLOT(onPipelineFrame(QImage)), Qt::QueuedConnection);
    connect(ap3216, SIGNAL(updateAp3216Data(QString,QString,QString)),
            this, SLOT(on_ap3216_data_updated(QString,QString,QString)));
    connect(ap3216, SIGNAL(availabilityChanged(bool)),
            this, SLOT(on_ap3216_availability_changed(bool)));
    connect(ap3216, SIGNAL(backendChanged(QString)),
            this, SLOT(on_ap3216_backend_changed(QString)));
    connect(ap3216, SIGNAL(proximityAlertChanged(bool,QString)),
            this, SLOT(on_ap3216_alert_changed(bool,QString)));

    // Auto-start media source when RTSP is enabled so frames flow
    // to the encoder even when the Monitor page is not displayed.
    if (m_rtspServer && m_rtspServer->isRunning()) {
        std::string errorMessage;
        if (!ap3216->startReading(&errorMessage)) {
            VLOG_WARN(MODULE, "ap3216 auto-start failed: %s",
                      errorMessage.c_str());
        }
        if (m_usePipeline) {
            startPipelineSource();
        } else {
            // Front remains the persistent RKNN/RTSP source; Rear starts in
            // parallel and is consumed locally only when reverse is active.
            startV4l2Cameras();
        }
    }
}

Monitor::~Monitor()
{
    if (m_pipelineStatsTimer) {
        m_pipelineStatsTimer->stop();
    }
    if (m_frameWatchdog) {
        m_frameWatchdog->stop();
        delete m_frameWatchdog;
        m_frameWatchdog = nullptr;
    }
    if (m_testFrameTimer) {
        m_testFrameTimer->stop();
        delete m_testFrameTimer;
        m_testFrameTimer = nullptr;
    }
    stopV4l2Cameras();
    // Stop and delete the pipeline before RTSP so the encode thread exits
    // cleanly before the encoder is torn down.
    if (m_pipeline) {
        m_pipeline->stop();
        delete m_pipeline;
        m_pipeline = nullptr;
    }
    // Stop the RKNN worker thread before deinitDetector so the worker is
    // not calling rknn::detect() while we release the detector.
    m_rknnThreadRunning = false;
    m_rknnCv.notify_all();
    if (m_rknnThread.joinable()) {
        m_rknnThread.join();
    }
    if (m_rknnReady) {
        rknn::deinitDetector();
        m_rknnReady = false;
    }
    if (m_rtspServer) {
        m_rtspServer->deinit();
        delete m_rtspServer;
        m_rtspServer = nullptr;
    }
    // The singleton is manually owned. Clear the static pointer so it cannot
    // retain a dangling address after explicit destruction.
    monitor = nullptr;

    delete ui;
}


void Monitor::showImage(QImage image)
{
    // Front remains alive even when Rear is selected for local display.
    m_cameraFrameReceived = true;
    if (m_cameraRouter) {
        m_cameraRouter->setAvailable(vehiclecamera::CameraRole::Front, true);
    }

    // Ensure RGB32 format (RTSP encoder and RKNN both expect RGB32).
    QImage rgb32Image = image;
    if (image.format() != QImage::Format_RGB32) {
        rgb32Image = image.convertToFormat(QImage::Format_RGB32);
    }

    // Push to RTSP stream (encoder runs on its own thread, non-blocking).
    if (m_rtspServer && m_rtspServer->isRunning()) {
        m_rtspServer->pushFrame(rgb32Image.bits(),
                                rgb32Image.width(), rgb32Image.height());
    }

    // Draw the last detection result on the current frame. Detection runs
    // asynchronously on m_rknnThread; the boxes may lag 1-3 frames behind
    // the live image, which is acceptable for vehicle alert scenarios.
    // This keeps showImage ~15ms/frame instead of ~95ms (80ms RKNN blocked).
    if (m_rknnReady && !m_lastDetections.empty()) {
        rknn::drawDetections(rgb32Image.bits(),
                             rgb32Image.width(), rgb32Image.height(),
                             m_lastDetections, rknn::cocoClassNames());
    }

    if (!m_cameraRouter ||
        m_cameraRouter->displayRole() == vehiclecamera::CameraRole::Front) {
        videoLabel->setPixmap(QPixmap::fromImage(rgb32Image));
    }

    // Feed frame to the RKNN worker thread (drop-olatest: only the newest
    // frame is kept, so the worker always processes the latest image and
    // never accumulates a backlog).
    if (m_rknnReady) {
        {
            std::lock_guard<std::mutex> lock(m_rknnMutex);
            m_rknnPendingFrame = rgb32Image.copy();
            m_rknnHasFrame = true;
        }
        m_rknnCv.notify_one();
    }
}

void Monitor::showRearImage(QImage image)
{
    // A queued frame can arrive after REVERSE OFF. Never allow that stale
    // frame to mark Rear available or switch the display back to Rear.
    if (!m_cameraRouter || !m_cameraRouter->reverseActive())
        return;

    m_rearFrameReceived = true;
    m_cameraRouter->setAvailable(vehiclecamera::CameraRole::Rear, true);

    // W10-P1 Rear is GUI-only: never feed Rear into Front RKNN or RTSP.
    if (m_cameraRouter->displayRole() !=
        vehiclecamera::CameraRole::Rear) {
        return;
    }

    QImage rgb32Image = image;
    if (image.format() != QImage::Format_RGB32) {
        rgb32Image = image.convertToFormat(QImage::Format_RGB32);
    }
    videoLabel->setPixmap(QPixmap::fromImage(rgb32Image));
}

void Monitor::setReverseActive(bool active)
{
    if (!m_cameraRouter)
        return;

    m_cameraRouter->setReverseActive(active);
    updateCameraRoleLabel();
}

void Monitor::handleRearCaptureRequest(bool active)
{
    if (!m_cameraRouter)
        return;

    if (!active) {
        m_cameraRouter->setAvailable(
            vehiclecamera::CameraRole::Rear, false);
        m_rearFrameReceived = false;

        if (m_rearCaptureModule &&
            m_rearCaptureModule->isCapturing()) {
            m_rearCaptureModule->stopCapture();
            VLOG_INFO(MODULE, "Rear camera capture stopped");
        }
        return;
    }

    // Keep Front displayed until Rear has delivered its first valid frame.
    m_cameraRouter->setAvailable(
        vehiclecamera::CameraRole::Rear, false);
    m_rearFrameReceived = false;

    if (m_usePipeline ||
        !m_rearCameraConfig.enabled ||
        m_rearCameraConfig.sourceType != "v4l2" ||
        !m_rearCaptureModule) {
        VLOG_WARN(MODULE,
                  "Rear capture request rejected: invalid configuration");
        return;
    }

    if (m_rearCaptureModule->isCapturing()) {
        VLOG_INFO(MODULE, "Rear camera capture already running");
        return;
    }

    if (!m_rearCaptureModule->startCapture()) {
        VLOG_WARN(MODULE, "Rear camera start failed: %s",
                  m_rearCaptureModule->lastErrorMessage().c_str());
        return;
    }

    VLOG_INFO(MODULE, "Rear camera capture started: %s",
              m_rearCameraConfig.device.c_str());
}

void Monitor::updateCameraRoleLabel()
{
    if (!cameraRoleLabel || !m_cameraRouter)
        return;

    const bool reverse = m_cameraRouter->reverseActive();
    const bool rearAvailable =
        m_cameraRouter->isAvailable(vehiclecamera::CameraRole::Rear);
    const auto role = m_cameraRouter->displayRole();

    if (reverse && !rearAvailable) {
        cameraRoleLabel->setText(
            QStringLiteral("倒车：后摄不可用，保持前摄 (FRONT FALLBACK)"));
    } else if (role == vehiclecamera::CameraRole::Rear) {
        cameraRoleLabel->setText(QStringLiteral("倒车影像 · REAR · IMX335 / CAM1"));
    } else {
        cameraRoleLabel->setText(QStringLiteral("前方影像 · FRONT · IMX415 / CAM3"));
    }
}

void Monitor::startV4l2Cameras()
{
    if (m_usePipeline)
        return;

    bool frontRunning = m_captureModule && m_captureModule->isCapturing();
    if (!frontRunning && m_cameraConfig.enabled && m_captureModule) {
        m_cameraFrameReceived = false;
        frontRunning = m_captureModule->startCapture();
        if (!frontRunning) {
            VLOG_WARN(MODULE, "Front camera start failed: %s",
                      m_captureModule->lastErrorMessage().c_str());
        }
    }

    if (!frontRunning) {
        if (m_cameraRouter) {
            m_cameraRouter->setAvailable(vehiclecamera::CameraRole::Front,
                                         false);
        }
        startTestFrameGenerator();
    } else {
        VLOG_INFO(MODULE, "Front camera capture running: %s",
                  m_cameraConfig.device.c_str());
        if (!m_frameWatchdog) {
            m_frameWatchdog = new QTimer(this);
            m_frameWatchdog->setSingleShot(true);
            connect(m_frameWatchdog, SIGNAL(timeout()),
                    this, SLOT(onFrameWatchdog()));
        }
        m_frameWatchdog->start(2000);
    }

    // Rear is event-driven. Merely opening Monitor must not power IMX335.
    if (m_cameraRouter && !m_cameraRouter->reverseActive()) {
        m_cameraRouter->setAvailable(
            vehiclecamera::CameraRole::Rear, false);
    }
}

void Monitor::stopV4l2Cameras()
{
    if (m_captureModule && m_captureModule->isCapturing()) {
        m_captureModule->stopCapture();
    }
    if (m_rearCaptureModule && m_rearCaptureModule->isCapturing()) {
        m_rearCaptureModule->stopCapture();
    }

    if (m_cameraRouter) {
        m_cameraRouter->setAvailable(vehiclecamera::CameraRole::Front, false);
        m_cameraRouter->setAvailable(vehiclecamera::CameraRole::Rear, false);
    }
}

void Monitor::rknnWorkerLoop()
{
    while (m_rknnThreadRunning) {
        QImage frame;
        {
            std::unique_lock<std::mutex> lock(m_rknnMutex);
            m_rknnCv.wait(lock, [this] {
                return m_rknnHasFrame || !m_rknnThreadRunning;
            });
            if (!m_rknnThreadRunning) break;
            frame = std::move(m_rknnPendingFrame);
            m_rknnHasFrame = false;
        }

        // Run YOLOv5 detection (~80ms). This no longer blocks the GUI
        // event loop; the worker processes frames at its own pace and
        // drop-olatest ensures it always works on the latest frame.
        std::vector<rknn::Detection> detections;
        const bool detectionSucceeded = rknn::detect(
            frame.bits(), frame.width(), frame.height(), detections);

        std::vector<vehiclealarm::DangerCandidate> dangerCandidates;
        if (detectionSucceeded) {
            dangerCandidates.reserve(detections.size());
            for (const rknn::Detection &detection : detections) {
                dangerCandidates.push_back({
                    detection.classId,
                    static_cast<double>(detection.w),
                    static_cast<double>(detection.h)
                });
            }
        }

        const vehiclealarm::DetectorFrameStatus frameStatus =
            detectionSucceeded
                ? vehiclealarm::DetectorFrameStatus::Succeeded
                : vehiclealarm::DetectorFrameStatus::Failed;
        const bool dangerChanged =
            m_dangerAlarmPolicy.update(frameStatus, dangerCandidates);
        const bool dangerActive = m_dangerAlarmPolicy.isActive();

        // Deliver detection results back to the GUI thread (queued) so
        // the next showImage call can draw boxes. dangerStateChanged is
        // emitted from the GUI thread to keep signal delivery consistent.
        QMetaObject::invokeMethod(this,
            [this, detections, dangerChanged, dangerActive]() mutable {
                m_lastDetections = std::move(detections);
                if (dangerChanged) {
                    emit dangerStateChanged(dangerActive);
                }
            }, Qt::QueuedConnection);
    }
}

void Monitor::resizeEvent(QResizeEvent *event)
{
    Q_UNUSED(event)
    if (startCaptureButton == nullptr ||
        ap3216StatusLabel == nullptr ||
        ap3216AlertLabel == nullptr ||
        cameraRoleLabel == nullptr) {
        return;
    }
    startCaptureButton->move((this->width() - 200) / 2, this->height() - 80);
    startCaptureButton->resize(200, 40);
    ap3216StatusLabel->resize(300, 150);
    ap3216StatusLabel->move(20, 20);
    ap3216AlertLabel->resize(240, 52);
    ap3216AlertLabel->move(this->width() - 260, 20);
    cameraRoleLabel->resize(360, 52);
    cameraRoleLabel->move((this->width() - 360) / 2, 20);
    // Re-raise after geometry changes so controls stay above videoLabel.
    startCaptureButton->raise();
    ap3216StatusLabel->raise();
    ap3216AlertLabel->raise();
    cameraRoleLabel->raise();
}

void Monitor::keyPressEvent(QKeyEvent *event)
{
    // Escape / Back exits the Monitor page. This is a fallback for the case
    // where the "返回" button is obscured or the pointer is unavailable; it
    // reuses the same stop+hide path as the button click.
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Back) {
        VLOG_INFO(MODULE, "Esc/Back key pressed; stopping monitor page");
        if (m_usePipeline || !m_rtspServer || !m_rtspServer->isRunning()) {
            myStop();
        }
        this->hide();
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

void Monitor::myStart()
{
    std::string errorMessage;
    if (!ap3216->startReading(&errorMessage)) {
        VLOG_WARN(MODULE, "ap3216 start failed: %s", errorMessage.c_str());
    }
    if (m_usePipeline) {
        startPipelineSource();
    } else {
        startV4l2Cameras();
    }
}

void Monitor::myStop()
{
    const bool keepPipelineForRtsp =
        m_usePipeline && m_rtspServer && m_rtspServer->isRunning();
    if (m_frameWatchdog) {
        m_frameWatchdog->stop();
    }
    if (m_testFrameTimer) {
        m_testFrameTimer->stop();
    }
    ap3216->stopReading();
    if (m_usePipeline) {
        // RTSP is a background service. Hiding the Monitor page must not
        // tear down its frame source; otherwise remote clients freeze.
        if (!keepPipelineForRtsp) {
            stopPipelineSource();
        } else {
            m_pipeline->setLaneEnabled(media::ConsumerLane::Gui, false);
            VLOG_INFO(MODULE,
                      "monitor hidden; GUI lane disabled and pipeline kept "
                      "running for RTSP");
        }
    } else {
        stopV4l2Cameras();
    }
    // The RKNN consumer may still be executing while RTSP keeps the pipeline
    // alive. Preserve both detector and alarm state until the pipeline stops.
    if (!keepPipelineForRtsp) {
        // Stop the RKNN worker thread BEFORE deinitDetector to avoid the
        // worker calling rknn::detect() on a released detector.
        m_rknnThreadRunning = false;
        m_rknnCv.notify_all();
        if (m_rknnThread.joinable()) {
            m_rknnThread.join();
        }
        if (m_dangerAlarmPolicy.reset()) {
            emit dangerStateChanged(false);
        }
        if (m_rknnReady) {
            rknn::deinitDetector();
            m_rknnReady = false;
        }
    }
}

// ---------------------------------------------------------------------------
// MediaPipeline source management (test_pattern / file sources).
//
// Creates the appropriate IFrameSource based on m_cameraConfig.sourceType,
// configures three independent consumer callbacks (GUI / RKNN / RTSP), and
// starts the pipeline. Each consumer runs on its own thread and has its own
// bounded queue so a slow consumer does not block the others.
// ---------------------------------------------------------------------------

void Monitor::startPipelineSource()
{
    if (!m_pipeline)
        return;

    // Reset the GUI frame throttle so a stale counter from a previous run
    // does not block the first frame.
    m_pendingGuiFrames.store(0);

    // RTSP may have auto-started the pipeline before the Monitor page opens.
    // setSource()/setConsumer*() are lifecycle operations and must never race
    // with running worker threads, so reuse the active pipeline here.
    if (m_pipeline->state() == media::MediaPipeline::State::Running) {
        m_pipeline->setLaneEnabled(media::ConsumerLane::Gui, true);
        if (m_pipelineStatsTimer && !m_pipelineStatsTimer->isActive())
            m_pipelineStatsTimer->start();
        VLOG_INFO(MODULE, "pipeline already running; reusing active source");
        return;
    }

    // Create the frame source based on configuration.
    std::unique_ptr<media::IFrameSource> source;
    const int w = m_cameraConfig.width;
    const int h = m_cameraConfig.height;
    const int fps = m_cameraConfig.fps;

    if (m_cameraConfig.sourceType == "test_pattern") {
        source = std::make_unique<media::TestPatternSource>(w, h, fps);
    } else if (m_cameraConfig.sourceType == "file") {
        source = std::make_unique<media::FileFrameSource>(
            m_cameraConfig.filePath, w, h, fps,
            m_cameraConfig.fileLoop);
    } else {
        VLOG_ERROR(MODULE, "startPipelineSource: unknown source type '%s'",
                   m_cameraConfig.sourceType.c_str());
        return;
    }

    m_pipeline->setSource(std::move(source));

    // Pre-allocate scratch buffers for NV12→RGB32 conversion (one per
    // consumer thread to avoid sharing).
    const size_t rgbSize = static_cast<size_t>(w) * h * 4;
    m_guiRgbBuffer.resize(rgbSize);
    m_rknnRgbBuffer.resize(rgbSize);
    m_rtspRgbBuffer.resize(rgbSize);

    // GUI consumer: NV12→RGB32 → emit queued signal for display.
    // The QImage is constructed from a deep copy so the scratch buffer can
    // be reused on the next frame without corrupting the displayed image.
    //
    // Frame throttling: at most one pending pipelineFrameReady event is
    // allowed in the Qt event queue. While one frame is still queued for the
    // GUI thread, newer frames are dropped (drop-latest) so mouse/key events
    // are not starved at 30 fps. Without this guard the queued frame events
    // pile up and the "返回" button becomes unclickable.
    m_pipeline->setGuiConsumer([this](const media::FramePacket &frame) {
        if (!frame.data || frame.width <= 0 || frame.height <= 0)
            return;
        // Drop-latest: if the GUI thread has not yet consumed the previous
        // frame, skip this one. fetch_add(1) returns the old value; if it was
        // already >=1, a frame is still pending.
        if (m_pendingGuiFrames.fetch_add(1) >= 1) {
            m_pendingGuiFrames.fetch_sub(1);
            return;
        }
        const size_t need = static_cast<size_t>(frame.width) * frame.height * 4;
        if (m_guiRgbBuffer.size() < need)
            m_guiRgbBuffer.resize(need);
        if (!rga::nv12ToRgb32(frame.data->data(), frame.width, frame.height,
                              m_guiRgbBuffer.data())) {
            m_pendingGuiFrames.fetch_sub(1);
            return;
        }
        QImage img(m_guiRgbBuffer.data(), frame.width, frame.height,
                   QImage::Format_RGB32);
        // Copy before emitting across threads — the scratch buffer will be
        // reused by the next frame.
        emit pipelineFrameReady(img.copy());
    });

    // RKNN consumer: NV12→RGB32 → detect → update alarm policy.
    media::FrameConsumer rknnConsumer;
    if (m_rknnReady) {
        rknnConsumer = [this](const media::FramePacket &frame) {
        if (!m_rknnReady || !frame.data)
            return;
        const size_t need = static_cast<size_t>(frame.width) * frame.height * 4;
        if (m_rknnRgbBuffer.size() < need)
            m_rknnRgbBuffer.resize(need);
        if (!rga::nv12ToRgb32(frame.data->data(), frame.width, frame.height,
                              m_rknnRgbBuffer.data()))
            return;
        std::vector<rknn::Detection> detections;
        const bool ok = rknn::detect(m_rknnRgbBuffer.data(),
                                     frame.width, frame.height, detections);
        std::vector<vehiclealarm::DangerCandidate> candidates;
        if (ok) {
            candidates.reserve(detections.size());
            for (const auto &d : detections) {
                candidates.push_back({d.classId,
                                      static_cast<double>(d.w),
                                      static_cast<double>(d.h)});
            }
        }
        const auto status = ok
            ? vehiclealarm::DetectorFrameStatus::Succeeded
            : vehiclealarm::DetectorFrameStatus::Failed;
        if (m_dangerAlarmPolicy.update(status, candidates)) {
            // Alarm state transition — emit via queued connection to the
            // GUI thread (dangerStateChanged is connected in mainwindow).
            emit dangerStateChanged(m_dangerAlarmPolicy.isActive());
        }
        };
    }
    m_pipeline->setRknnConsumer(std::move(rknnConsumer));

    // RTSP consumer: NV12→RGB32 → push to encoder.
    media::FrameConsumer rtspConsumer;
    if (m_rtspServer && m_rtspServer->isRunning()) {
        rtspConsumer = [this](const media::FramePacket &frame) {
        if (!m_rtspServer || !m_rtspServer->isRunning())
            return;
        if (!frame.data)
            return;
        const size_t need = static_cast<size_t>(frame.width) * frame.height * 4;
        if (m_rtspRgbBuffer.size() < need)
            m_rtspRgbBuffer.resize(need);
        if (!rga::nv12ToRgb32(frame.data->data(), frame.width, frame.height,
                              m_rtspRgbBuffer.data()))
            return;
        m_rtspServer->pushFrame(m_rtspRgbBuffer.data(),
                                frame.width, frame.height);
        };
    }
    m_pipeline->setRtspConsumer(std::move(rtspConsumer));

    std::string error;
    if (!m_pipeline->start(&error)) {
        VLOG_ERROR(MODULE, "pipeline start failed: %s", error.c_str());
        videoLabel->setText(QString("媒体源启动失败: %1")
                                .arg(QString::fromStdString(error)));
    } else {
        if (m_pipelineStatsTimer)
            m_pipelineStatsTimer->start();
        VLOG_INFO(MODULE, "pipeline started (source=%s %dx%d@%d)",
                  m_cameraConfig.sourceType.c_str(), w, h, fps);
    }
}

void Monitor::stopPipelineSource()
{
    if (m_pipeline) {
        m_pipeline->stop();
        onPipelineStats();
    }
    if (m_pipelineStatsTimer)
        m_pipelineStatsTimer->stop();
    // Clear the throttle so a late onPipelineFrame (queued before stop) does
    // not leave the counter positive for the next start.
    m_pendingGuiFrames.store(0);
    VLOG_INFO(MODULE, "pipeline stopped");
}

void Monitor::onPipelineStats()
{
    if (!m_pipeline)
        return;

    const media::MediaPipelineStatistics stats = m_pipeline->statistics();
    const auto logLane = [&stats](
                             const char *name,
                             const media::MediaConsumerStatistics &lane) {
        VLOG_INFO(MODULE,
                  "[MEDIA_STATS] %s frames=%.0f fps=%.2f drops=%.0f "
                  "queue=%d/%d max=%d process_ms(avg=%.2f,p95=%.2f)",
                  name,
                  static_cast<double>(lane.processedFrames),
                  lane.fps,
                  static_cast<double>(lane.droppedFrames),
                  lane.queueDepth, stats.queueCapacity, lane.maxQueueDepth,
                  lane.averageProcessingMs, lane.p95ProcessingMs);
    };

    VLOG_INFO(MODULE,
              "[MEDIA_STATS] source frames=%.0f fps=%.2f runtime=%.2fs "
              "queue_capacity=%d",
              static_cast<double>(stats.sourceFrames),
              stats.sourceFps, stats.runtimeSeconds, stats.queueCapacity);
    logLane("gui", stats.gui);
    logLane("rknn", stats.rknn);
    logLane("rtsp", stats.rtsp);
}

void Monitor::onPipelineFrame(QImage frame)
{
    // Runs on the GUI thread (queued connection). Safe to update QWidget.
    // IMPORTANT: release the throttle slot ONLY after setPixmap completes.
    // If fetch_sub(1) runs before setPixmap, the worker thread sees the slot
    // as free while the GUI thread is still busy rendering, and immediately
    // emits the next frame. At 30 fps (frame interval 33ms < setPixmap cost)
    // this causes the Qt event queue to always have one pending frame event,
    // starving timer/mouse/key events — the 5s MEDIA_STATS timer never fires,
    // Esc does not respond, the "返回" button does not respond.
    // Holding the slot through setPixmap guarantees at most one frame is ever
    // in the queue, so the GUI thread has idle time between frames to process
    // other events.
    m_cameraFrameReceived = true;
    videoLabel->setPixmap(QPixmap::fromImage(frame));
    m_pendingGuiFrames.fetch_sub(1);
}

void Monitor::startCaptureButtonClicked(bool start)
{
    Q_UNUSED(start);
    // Log at slot entry so logs can distinguish "button never got the click"
    // from "click received but stop/hide failed". Previously there was no
    // entry log, which made the 30 fps unclickable symptom ambiguous.
    VLOG_INFO(MODULE, "返回 button clicked; stopping monitor page");
    // Software-pipeline myStop() disables only the GUI lane while RTSP is
    // active. Keep the legacy V4L2 behavior unchanged until that path also
    // uses independently gated consumer lanes.
    if (m_usePipeline || !m_rtspServer || !m_rtspServer->isRunning()) {
        myStop();
    }
    this->hide();
}

void Monitor::refreshAp3216Panel()
{
    const QString availability =
        ap3216Available ? QStringLiteral("可用") : QStringLiteral("不可用");
    ap3216StatusLabel->setText(
        QStringLiteral("AP3216C 环境监测\n"
                       "后端: %1\n"
                       "状态: %2\n"
                       "光照(raw): %3\n"
                       "接近(raw): %4\n"
                       "红外(raw): %5")
            .arg(ap3216Backend, availability,
                 ap3216Als, ap3216Ps, ap3216Ir));

    if (!ap3216Available) {
        ap3216AlertLabel->setText(QStringLiteral("接近告警：传感器不可用"));
        ap3216AlertLabel->setStyleSheet(
            "QLabel { color: white; background-color: rgba(90,90,90,210); "
            "border-radius: 6px; padding: 6px; }");
    } else if (ap3216AlertActive) {
        ap3216AlertLabel->setText(
            QStringLiteral("接近告警：触发 (%1)").arg(ap3216AlertSource));
        ap3216AlertLabel->setStyleSheet(
            "QLabel { color: white; background-color: rgba(190,30,30,230); "
            "border-radius: 6px; padding: 6px; }");
    } else {
        ap3216AlertLabel->setText(
            QStringLiteral("接近告警：正常 (%1)").arg(ap3216AlertSource));
        ap3216AlertLabel->setStyleSheet(
            "QLabel { color: white; background-color: rgba(20,130,70,220); "
            "border-radius: 6px; padding: 6px; }");
    }
}

void Monitor::on_ap3216_data_updated(QString als, QString ps, QString ir)
{
    ap3216Als = als;
    ap3216Ps = ps;
    ap3216Ir = ir;
    refreshAp3216Panel();
}

void Monitor::on_ap3216_availability_changed(bool available)
{
    ap3216Available = available;
    refreshAp3216Panel();
}

void Monitor::on_ap3216_backend_changed(QString backend)
{
    ap3216Backend = backend;
    refreshAp3216Panel();
}

void Monitor::on_ap3216_alert_changed(bool active, QString source)
{
    ap3216AlertActive = active;
    ap3216AlertSource = source;
    refreshAp3216Panel();
}

void Monitor::on_handleCommand(int command)
{
    switch (command) {
    case MONITOR_COMMAND_SHOW:
        this->show();
        break;
    case MONITOR_COMMAND_CLOSE:
        this->hide();
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Frame watchdog — fires 2 s after camera start. If no real camera frames
// have arrived by then (e.g. STREAMON failed due to missing sensor),
// automatically switch to the test pattern generator.
// ---------------------------------------------------------------------------
void Monitor::onFrameWatchdog()
{
    if (!m_cameraFrameReceived) {
        VLOG_WARN(MODULE, "no camera frames after 2 s; "
                          "falling back to test pattern");
        startTestFrameGenerator();
    }
}

// ---------------------------------------------------------------------------
// Test pattern generator — used when camera hardware is not available.
// Produces a color bar pattern at 25 fps so the entire RTSP pipeline
// (encode → RTP → VLC) can be verified without a real camera.
// ---------------------------------------------------------------------------
void Monitor::startTestFrameGenerator()
{
    if (m_testFrameTimer && m_testFrameTimer->isActive())
        return;

    VLOG_INFO(MODULE, "starting Front fallback test pattern generator (25 fps)");
    if (m_testFrameTimer) {
        delete m_testFrameTimer;
        m_testFrameTimer = nullptr;
    }
    m_testFrameTimer = new QTimer(this);
    connect(m_testFrameTimer, SIGNAL(timeout()),
            this, SLOT(onTestFrameTick()));
    m_testFrameTimer->start(40); // 40 ms = 25 fps
}

void Monitor::onTestFrameTick()
{
    const int W = 800;
    const int H = 600;
    QImage frame(W, H, QImage::Format_RGB32);
    frame.fill(Qt::black);

    // Draw 8 vertical color bars (white, yellow, cyan, green,
    // magenta, red, blue, black).
    static const QColor bars[] = {
        Qt::white, Qt::yellow, Qt::cyan, Qt::green,
        Qt::magenta, Qt::red, Qt::blue, Qt::black
    };
    const int barW = W / 8;
    QPainter painter(&frame);
    for (int i = 0; i < 8; i++) {
        painter.fillRect(i * barW, 0, barW, H, bars[i]);
    }

    // Draw a moving white square to show motion.
    int x = (m_testFrameIndex * 6) % (W - 80);
    int y = H / 2 - 40 + static_cast<int>(
        100 * std::sin(m_testFrameIndex * 0.1));
    painter.fillRect(x, y, 80, 80, Qt::white);

    // Draw frame counter text.
    painter.setPen(Qt::black);
    painter.setFont(QFont("Monospace", 18));
    painter.drawText(10, 30,
                     QString("TEST #%1  RTSP:%2")
                         .arg(m_testFrameIndex)
                         .arg(m_rtspServer
                              ? m_rtspServer->clientCount()
                              : 0));
    painter.end();

    m_testFrameIndex++;

    // Treat software pattern as Front fallback. showImage() keeps Front
    // RKNN/RTSP alive while CameraRouter controls local Front/Rear display.
    showImage(frame);
}
