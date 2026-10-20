#include "capture_thread.h"
#include "cameraconfig.h"
#include "logmanager.h"
#include "rga_converter.h"
#include "mpp_decoder.h"
#include "rknn_detector.h"
#include "v4l2_capture_utils.h"

#include <chrono>
#include <cstring>
#include <cstdio>
#include <cerrno>

#ifdef linux
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <poll.h>
#endif

static const char *MODULE = "camera";

// ---------------------------------------------------------------------------
// State / error code strings
// ---------------------------------------------------------------------------

const char *CaptureThread::stateToString(State state)
{
    switch (state) {
    case State::Uninitialized: return "Uninitialized";
    case State::Ready:         return "Ready";
    case State::Starting:      return "Starting";
    case State::Capturing:     return "Capturing";
    case State::Stopping:      return "Stopping";
    case State::Error:         return "Error";
    }
    return "Unknown";
}

const char *CaptureThread::errorCodeToString(ErrorCode code)
{
    switch (code) {
    case ErrorCode::None:                return "None";
    case ErrorCode::DeviceOpenFailed:    return "DeviceOpenFailed";
    case ErrorCode::FormatNotSupported:  return "FormatNotSupported";
    case ErrorCode::BufferSetupFailed:   return "BufferSetupFailed";
    case ErrorCode::StreamStartFailed:   return "StreamStartFailed";
    case ErrorCode::FrameCaptureError:   return "FrameCaptureError";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

CaptureThread::CaptureThread(QObject *parent)
    : QThread(parent)
    , m_config(nullptr)
    , m_fd(-1)
    , m_pixelFormat(0)
    , m_bufferType(0)
    , m_multiplanarCapture(false)
    , m_actualWidth(0)
    , m_actualHeight(0)
    , m_lastTelemetryLogMonotonicNs(0)
    , m_state(State::Uninitialized)
    , m_lastError(ErrorCode::None)
    , m_stopRequested(false)
    , m_threadExited(false)
{
}

CaptureThread::~CaptureThread()
{
    shutdown();
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

CaptureThread::State CaptureThread::state() const
{
    QMutexLocker lock(&m_stateMutex);
    return m_state;
}

CaptureThread::ErrorCode CaptureThread::lastError() const
{
    QMutexLocker lock(&m_stateMutex);
    return m_lastError;
}

std::string CaptureThread::lastErrorMessage() const
{
    QMutexLocker lock(&m_stateMutex);
    return m_lastErrorMessage;
}

bool CaptureThread::isCapturing() const
{
    QMutexLocker lock(&m_stateMutex);
    return m_state == State::Capturing;
}

// ---------------------------------------------------------------------------
// V4L2 dequeue telemetry
// ---------------------------------------------------------------------------

std::uint64_t CaptureThread::monotonicNowNs()
{
    const auto now =
        std::chrono::steady_clock::now().time_since_epoch();

    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now).count());
}

void CaptureThread::resetFrameTelemetry()
{
    const std::uint64_t nowNs = monotonicNowNs();
    m_frameTelemetry.reset(nowNs);
    m_lastTelemetryLogMonotonicNs = nowNs;
}

void CaptureThread::observeFrameTelemetry(std::uint32_t sequence)
{
    const std::uint64_t nowNs = monotonicNowNs();
    m_frameTelemetry.observe(sequence, nowNs);

    constexpr std::uint64_t logPeriodNs = 5000000000ULL;

    if (nowNs >= m_lastTelemetryLogMonotonicNs &&
        nowNs - m_lastTelemetryLogMonotonicNs >= logPeriodNs) {
        logFrameTelemetry("periodic", nowNs);
        m_lastTelemetryLogMonotonicNs = nowNs;
    }
}

void CaptureThread::logFrameTelemetry(
    const char *reason,
    std::uint64_t nowMonotonicNs)
{
    const vehiclecamera::V4L2FrameTelemetrySnapshot stats =
        m_frameTelemetry.snapshot(nowMonotonicNs);

    const char *device =
        m_config ? m_config->device.c_str() : "unknown";

    VLOG_INFO(
        MODULE,
        "[V4L2_STATS] device=%s reason=%s frames=%llu fps=%.2f "
        "gaps=%llu seq=%u..%u max_interval_ms=%.3f",
        device,
        reason,
        static_cast<unsigned long long>(stats.frames),
        stats.fps,
        static_cast<unsigned long long>(stats.sequenceGaps),
        stats.firstSequence,
        stats.lastSequence,
        stats.maxFrameIntervalMs);
}

// ---------------------------------------------------------------------------
// State transition helpers
// ---------------------------------------------------------------------------

void CaptureThread::transitionTo(State newState)
{
    {
        QMutexLocker lock(&m_stateMutex);
        VLOG_INFO(MODULE, "state: %s -> %s", stateToString(m_state), stateToString(newState));
        m_state = newState;
    }
    // Wake startCapture() if it is waiting for Starting→Capturing/Error.
    m_startCondition.wakeAll();
    emit stateChanged(newState);
}

void CaptureThread::transitionToError(ErrorCode code, const std::string &message)
{
    {
        QMutexLocker lock(&m_stateMutex);
        m_lastError = code;
        m_lastErrorMessage = message;
        VLOG_ERROR(MODULE, "%s: %s", errorCodeToString(code), message.c_str());
        m_state = State::Error;
    }
    // Wake startCapture() if it is waiting for Starting→Capturing/Error.
    m_startCondition.wakeAll();
    emit stateChanged(State::Error);
}

// ---------------------------------------------------------------------------
// Lifecycle: init
// ---------------------------------------------------------------------------

bool CaptureThread::init(const vehiclecamera::CameraConfig &config)
{
    QMutexLocker lock(&m_stateMutex);

    if (m_state != State::Uninitialized) {
        VLOG_WARN(MODULE, "init() called in state %s, expected Uninitialized",
                  stateToString(m_state));
        return false;
    }

    if (!config.enabled) {
        VLOG_INFO(MODULE, "camera disabled by config, staying Uninitialized");
        return true;
    }

    if (config.device.empty()) {
        VLOG_ERROR(MODULE, "config error: camera_enabled=true but camera_device is empty");
        m_lastError = ErrorCode::DeviceOpenFailed;
        m_lastErrorMessage = "camera_device is empty";
        return false;
    }

    // Store config (make a heap copy so it outlives the caller).
    m_config = new vehiclecamera::CameraConfig(config);
    m_state = State::Ready;

    VLOG_INFO(MODULE, "init: device=%s %dx%d fmt=%s bufs=%d fps=%d",
              m_config->device.c_str(),
              m_config->width, m_config->height,
              m_config->format.c_str(),
              m_config->bufferCount, m_config->fps);

    return true;
}

// ---------------------------------------------------------------------------
// Lifecycle: startCapture
// ---------------------------------------------------------------------------

bool CaptureThread::startCapture()
{
    {
        QMutexLocker lock(&m_stateMutex);

        if (m_state == State::Uninitialized) {
            VLOG_WARN(MODULE, "startCapture() called before init()");
            return false;
        }

        if (m_state != State::Ready) {
            VLOG_WARN(MODULE, "startCapture() called in state %s, expected Ready",
                      stateToString(m_state));
            return false;
        }

        if (!m_config || !m_config->enabled) {
            VLOG_INFO(MODULE, "startCapture() skipped: camera disabled");
            return false;
        }

        m_state = State::Starting;
        m_threadExited = false;
    }

    {
        QMutexLocker stopLock(&m_stopMutex);
        m_stopRequested = false;
    }
    emit stateChanged(State::Starting);

    this->start();

    // Wait for the worker thread to finish real initialization (open device,
    // negotiate format, setup buffers, STREAMON) and transition to Capturing
    // or Error. This blocks startCapture() until the source is actually ready
    // to deliver frames — the previous code returned true immediately.
    QMutexLocker lock(&m_stateMutex);
    const unsigned long timeoutMs =
        m_config ? static_cast<unsigned long>(m_config->startTimeoutMs) : 5000;
    while (m_state == State::Starting) {
        if (!m_startCondition.wait(&m_stateMutex, timeoutMs)) {
            // Timed out waiting for initialization. Trigger a safe stop.
            VLOG_ERROR(MODULE, "startCapture() timed out after %lu ms", timeoutMs);
            m_state = State::Error;
            m_lastError = ErrorCode::StreamStartFailed;
            m_lastErrorMessage = "startCapture timed out";
            lock.unlock();
            // Stop the worker thread safely (no terminate).
            {
                QMutexLocker stopLock(&m_stopMutex);
                m_stopRequested = true;
            }
            if (this->isRunning())
                this->wait(timeoutMs);
            transitionTo(State::Ready);
            return false;
        }
    }

    if (m_state == State::Capturing)
        return true;

    // State::Error — initialization failed. Reset to Ready so the caller can
    // retry startCapture() after correcting the problem.
    VLOG_ERROR(MODULE, "startCapture() failed: %s",
               m_lastErrorMessage.c_str());

    // W10-P1B CaptureThread error-path self-deadlock fix:
    // 'lock' still owns m_stateMutex here. transitionTo() acquires the same
    // non-recursive QMutex internally, so calling it while the locker is held
    // deadlocks the caller after a worker-side STREAMON/open failure.
    lock.unlock();
    transitionTo(State::Ready);
    return false;
}

// ---------------------------------------------------------------------------
// Lifecycle: stopCapture
// ---------------------------------------------------------------------------

void CaptureThread::stopCapture()
{
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_state != State::Capturing && m_state != State::Starting) {
            VLOG_DEBUG(MODULE, "stopCapture() skipped: state is %s", stateToString(m_state));
            return;
        }
        m_state = State::Stopping;
    }
    emit stateChanged(State::Stopping);

    {
        QMutexLocker stopLock(&m_stopMutex);
        m_stopRequested = true;
    }
    // Wake any startCapture() waiting on the start condition so it can observe
    // the stop and return instead of hanging.
    m_startCondition.wakeAll();

    if (this->isRunning()) {
        // Use the configured stop timeout (default 3s). Do not force-kill
        // this thread: that leaves V4L2 buffers, MPP, RGA, mutexes and QObject
        // state inconsistent. The worker loop checks m_stopRequested
        // every poll iteration (≤100 ms) and exits promptly. If wait() times
        // out, log the failure but keep the thread handle; the destructor will
        // attempt one more join.
        const unsigned long timeoutMs =
            m_config ? static_cast<unsigned long>(m_config->stopTimeoutMs) : 3000;
        if (!this->wait(timeoutMs)) {
            VLOG_ERROR(MODULE,
                       "capture thread did not exit in %lu ms; "
                       "resource leak possible (forced shutdown disabled)",
                       timeoutMs);
        }
    }

    // Thread has exited (or timed out). Transition to Ready so startCapture()
    // can be called again. Resource cleanup is best-effort: the worker's run()
    // performs STREAMOFF/munmap/close/RGA/MPP cleanup before exiting.
    transitionTo(State::Ready);
}

// ---------------------------------------------------------------------------
// Lifecycle: shutdown
// ---------------------------------------------------------------------------

void CaptureThread::shutdown()
{
    stopCapture();

    releaseBuffers();
    closeDevice();

    delete m_config;
    m_config = nullptr;

    {
        QMutexLocker lock(&m_stateMutex);
        m_state = State::Uninitialized;
        m_lastError = ErrorCode::None;
        m_lastErrorMessage.clear();
    }

    VLOG_INFO(MODULE, "shutdown complete");
}

// ---------------------------------------------------------------------------
// Thread entry point
// ---------------------------------------------------------------------------

void CaptureThread::run()
{
#ifdef linux
    if (!openDevice()) {
        transitionToError(ErrorCode::DeviceOpenFailed,
                          "cannot open " + m_config->device);
        return;
    }

    if (!negotiateFormat()) {
        transitionToError(ErrorCode::FormatNotSupported,
                          "format negotiation failed for " + m_config->format);
        releaseBuffers();
        closeDevice();
        return;
    }

    if (!setupBuffers()) {
        transitionToError(ErrorCode::BufferSetupFailed, "buffer setup failed");
        closeDevice();
        return;
    }

    if (!startStream()) {
        transitionToError(ErrorCode::StreamStartFailed, "STREAMON failed");
        releaseBuffers();
        closeDevice();
        return;
    }

    resetFrameTelemetry();
    transitionTo(State::Capturing);

    VLOG_INFO(MODULE, "capturing: %s %dx%d fmt=0x%08x",
              m_config->device.c_str(),
              m_actualWidth, m_actualHeight, m_pixelFormat);

    // Initialize RGA hardware acceleration for NV12→RGB32 conversion.
    rga::init();

    // Initialize MPP decoder for MJPEG hardware decoding.
    mpp::initDecoder();

    // Pre-allocate RGB32 buffer for RGA/CPU conversion (avoids per-frame allocation).
    m_rgbBuffer.resize(m_actualWidth * m_actualHeight * 4);

    // Main capture loop.
    while (true) {
        {
            QMutexLocker stopLock(&m_stopMutex);
            if (m_stopRequested)
                break;
        }

        // 用 poll 等待帧就绪，避免 O_NONBLOCK + EAGAIN 导致的 100% CPU 忙等待。
        // 超时 100ms 以便及时响应 m_stopRequested。
        struct pollfd pfd;
        pfd.fd = m_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int pollRet = poll(&pfd, 1, 100);
        if (pollRet < 0) {
            if (errno == EINTR)
                continue; // 被信号中断，重试
            VLOG_ERROR(MODULE, "poll on capture fd failed: %s", strerror(errno));
            break;
        }
        if (pollRet == 0)
            continue; // 超时：无帧就绪，重新检查 m_stopRequested
        if (pfd.revents & (POLLERR | POLLNVAL)) {
            VLOG_ERROR(MODULE, "poll returned error condition (revents=0x%x)",
                       pfd.revents);
            break;
        }

        if (!dequeueAndEmitFrame()) {
            // Non-fatal: EAGAIN or transient error, continue.
        }
    }

    logFrameTelemetry("stream-stop", monotonicNowNs());
    stopStream();
    releaseBuffers();
    closeDevice();

    rga::deinit();
    mpp::deinitDecoder();

    VLOG_INFO(MODULE, "capture loop exited");
#else
    // V4L2 not available on this platform (e.g. Windows host). Transition
    // to Error so startCapture() returns false instead of hanging forever.
    transitionToError(ErrorCode::DeviceOpenFailed,
                      "V4L2 not available on this platform");
#endif
    {
        QMutexLocker lock(&m_stateMutex);
        m_threadExited = true;
    }
}

// ---------------------------------------------------------------------------
// V4L2 device operations
// ---------------------------------------------------------------------------

bool CaptureThread::openDevice()
{
#ifdef linux
    m_fd = open(m_config->device.c_str(), O_RDWR | O_NONBLOCK);
    if (m_fd < 0) {
        VLOG_ERROR(MODULE, "open(%s) failed: %s",
                   m_config->device.c_str(), strerror(errno));
        return false;
    }

    struct v4l2_capability cap;
    if (ioctl(m_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        VLOG_ERROR(MODULE, "VIDIOC_QUERYCAP failed: %s", strerror(errno));
        closeDevice();
        return false;
    }

    vehiclecamera::V4L2CaptureSelection captureSelection;
    if (!vehiclecamera::selectV4L2CaptureBufferType(cap.capabilities,
                                                    cap.device_caps,
                                                    &captureSelection)) {
        VLOG_ERROR(MODULE, "device does not support streaming video capture");
        closeDevice();
        return false;
    }

    m_bufferType = captureSelection.bufferType;
    m_multiplanarCapture = captureSelection.multiplanar;

    VLOG_DEBUG(MODULE, "opened %s (driver: %s, buffers: %s)",
               m_config->device.c_str(), cap.driver,
               vehiclecamera::v4l2BufferTypeName(m_bufferType));
    return true;
#else
    return false;
#endif
}

bool CaptureThread::negotiateFormat()
{
#ifdef linux
    bool found = false;
    uint32_t desiredFmt =
        vehiclecamera::cameraFormatToV4L2Fourcc(m_config->format,
                                                V4L2_PIX_FMT_NV12,
                                                &found);
    if (!found) {
        VLOG_WARN(MODULE, "unknown format '%s', falling back to NV12",
                  m_config->format.c_str());
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = static_cast<v4l2_buf_type>(m_bufferType);
    if (m_multiplanarCapture) {
        fmt.fmt.pix_mp.width = m_config->width;
        fmt.fmt.pix_mp.height = m_config->height;
        fmt.fmt.pix_mp.pixelformat = desiredFmt;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    } else {
        fmt.fmt.pix.width = m_config->width;
        fmt.fmt.pix.height = m_config->height;
        fmt.fmt.pix.pixelformat = desiredFmt;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
    }

    if (ioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0) {
        VLOG_ERROR(MODULE, "VIDIOC_S_FMT failed: %s", strerror(errno));
        return false;
    }

    if (m_multiplanarCapture) {
        m_pixelFormat = fmt.fmt.pix_mp.pixelformat;
        m_actualWidth = fmt.fmt.pix_mp.width;
        m_actualHeight = fmt.fmt.pix_mp.height;
    } else {
        m_pixelFormat = fmt.fmt.pix.pixelformat;
        m_actualWidth = fmt.fmt.pix.width;
        m_actualHeight = fmt.fmt.pix.height;
    }

    VLOG_INFO(MODULE, "format negotiated: %c%c%c%c %dx%d (requested %dx%d %s, %s)",
              m_pixelFormat & 0xff,
              (m_pixelFormat >> 8) & 0xff,
              (m_pixelFormat >> 16) & 0xff,
              (m_pixelFormat >> 24) & 0xff,
              m_actualWidth, m_actualHeight,
              m_config->width, m_config->height,
              m_config->format.c_str(),
              vehiclecamera::v4l2BufferTypeName(m_bufferType));

    // Set frame rate (best-effort, not all drivers support this).
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = static_cast<v4l2_buf_type>(m_bufferType);
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = m_config->fps;
    if (ioctl(m_fd, VIDIOC_S_PARM, &parm) < 0) {
        VLOG_DEBUG(MODULE, "VIDIOC_S_PARM not supported, using driver default FPS");
    }

    return true;
#else
    return false;
#endif
}

bool CaptureThread::setupBuffers()
{
#ifdef linux
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = m_config->bufferCount;
    req.type = static_cast<v4l2_buf_type>(m_bufferType);
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(m_fd, VIDIOC_REQBUFS, &req) < 0) {
        VLOG_ERROR(MODULE, "VIDIOC_REQBUFS failed: %s", strerror(errno));
        return false;
    }

    if (req.count < 2) {
        VLOG_ERROR(MODULE, "insufficient buffers granted: %u (need >= 2)", req.count);
        return false;
    }

    m_buffers.resize(req.count);

    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type = static_cast<v4l2_buf_type>(m_bufferType);
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (m_multiplanarCapture) {
            buf.m.planes = planes;
            buf.length = 1;
        }

        if (ioctl(m_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            VLOG_ERROR(MODULE, "VIDIOC_QUERYBUF[%u] failed: %s", i, strerror(errno));
            return false;
        }

        const unsigned int length = m_multiplanarCapture ? planes[0].length : buf.length;
        const unsigned int offset = m_multiplanarCapture ? planes[0].m.mem_offset : buf.m.offset;

        m_buffers[i].length = length;
        m_buffers[i].start = mmap(nullptr, length,
                                  PROT_READ | PROT_WRITE, MAP_SHARED,
                                  m_fd, offset);

        if (m_buffers[i].start == MAP_FAILED) {
            VLOG_ERROR(MODULE, "mmap[%u] failed: %s (length=%u)",
                       i, strerror(errno), length);
            m_buffers[i].start = nullptr;
            return false;
        }
    }

    VLOG_INFO(MODULE, "%llu buffers mapped",
              static_cast<unsigned long long>(m_buffers.size()));
    return true;
#else
    return false;
#endif
}

bool CaptureThread::startStream()
{
#ifdef linux
    for (unsigned int i = 0; i < m_buffers.size(); i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type = static_cast<v4l2_buf_type>(m_bufferType);
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (m_multiplanarCapture) {
            buf.m.planes = planes;
            buf.length = 1;
        }

        if (ioctl(m_fd, VIDIOC_QBUF, &buf) < 0) {
            VLOG_ERROR(MODULE, "VIDIOC_QBUF[%u] failed: %s", i, strerror(errno));
            return false;
        }
    }

    enum v4l2_buf_type type = static_cast<v4l2_buf_type>(m_bufferType);
    if (ioctl(m_fd, VIDIOC_STREAMON, &type) < 0) {
        VLOG_ERROR(MODULE, "VIDIOC_STREAMON failed: %s", strerror(errno));
        return false;
    }

    VLOG_INFO(MODULE, "stream started");
    return true;
#else
    return false;
#endif
}

void CaptureThread::stopStream()
{
#ifdef linux
    if (m_fd < 0)
        return;
    enum v4l2_buf_type type = static_cast<v4l2_buf_type>(m_bufferType);
    if (ioctl(m_fd, VIDIOC_STREAMOFF, &type) < 0) {
        VLOG_WARN(MODULE, "VIDIOC_STREAMOFF failed: %s", strerror(errno));
    } else {
        VLOG_INFO(MODULE, "stream stopped");
    }
#endif
}

void CaptureThread::releaseBuffers()
{
#ifdef linux
    for (auto &slot : m_buffers) {
        if (slot.start && slot.start != MAP_FAILED) {
            munmap(slot.start, slot.length);
        }
        slot.start = nullptr;
        slot.length = 0;
    }
    m_buffers.clear();
#endif
}

void CaptureThread::closeDevice()
{
#ifdef linux
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
        VLOG_DEBUG(MODULE, "device closed");
    }
#endif
}

// ---------------------------------------------------------------------------
// Frame dequeue and conversion
// ---------------------------------------------------------------------------

bool CaptureThread::dequeueAndEmitFrame()
{
#ifdef linux
    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = static_cast<v4l2_buf_type>(m_bufferType);
    buf.memory = V4L2_MEMORY_MMAP;
    if (m_multiplanarCapture) {
        buf.m.planes = planes;
        buf.length = 1;
    }

    if (ioctl(m_fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN)
            return true; // no frame ready yet, not an error
        VLOG_ERROR(MODULE, "VIDIOC_DQBUF failed: %s", strerror(errno));
        emit frameError(QString("DQBUF failed: %1").arg(strerror(errno)));
        return false;
    }

    observeFrameTelemetry(buf.sequence);

    bool frameOk = false;
    const unsigned int bytesUsed =
        m_multiplanarCapture ? planes[0].bytesused : buf.bytesused;

    if (buf.index < m_buffers.size() && bytesUsed > 0) {
        const uint8_t *data =
            static_cast<const uint8_t *>(m_buffers[buf.index].start);

        if (m_pixelFormat == V4L2_PIX_FMT_NV12 &&
            rga::nv12ToRgb32(data, m_actualWidth, m_actualHeight,
                             m_rgbBuffer.data())) {
            QImage frame(m_rgbBuffer.data(), m_actualWidth, m_actualHeight,
                         QImage::Format_RGB32);
            emit imageReady(frame.copy());
            frameOk = true;
        } else if (m_pixelFormat == V4L2_PIX_FMT_NV21) {
            QImage frame = nv21ToRgb(data, m_actualWidth, m_actualHeight);
            if (!frame.isNull()) {
                emit imageReady(frame.copy());
                frameOk = true;
            }
        } else if (m_pixelFormat == V4L2_PIX_FMT_UYVY) {
            QImage frame = uyvyToRgb(data, m_actualWidth, m_actualHeight);
            if (!frame.isNull()) {
                emit imageReady(frame.copy());
                frameOk = true;
            }
        } else if (m_pixelFormat == V4L2_PIX_FMT_YUYV) {
            QImage frame = yuyvToRgb(data, m_actualWidth, m_actualHeight);
            if (!frame.isNull()) {
                emit imageReady(frame.copy());
                frameOk = true;
            }
        } else if (m_pixelFormat == V4L2_PIX_FMT_MJPEG) {
            // MJPEG: use MPP hardware decoder → NV12, then RGA → RGB32.
            const size_t nv12BufSize = static_cast<size_t>(m_actualWidth) *
                                       m_actualHeight * 3 / 2;
            m_nv12Buffer.resize(nv12BufSize);
            int decodedW = 0, decodedH = 0;
            if (mpp::decode(data, bytesUsed,
                            m_nv12Buffer.data(), m_nv12Buffer.size(),
                            decodedW, decodedH)) {
                // Resize RGB buffer to match actual decoded dimensions (defense
                // against cameras whose MJPEG resolution differs from V4L2
                // negotiated dimensions).
                const size_t rgbBufSize = static_cast<size_t>(decodedW) *
                                          decodedH * 4;
                if (rgbBufSize > m_rgbBuffer.size())
                    m_rgbBuffer.resize(rgbBufSize);
                if (rga::nv12ToRgb32(m_nv12Buffer.data(), decodedW, decodedH,
                                     m_rgbBuffer.data())) {
                    QImage frame(m_rgbBuffer.data(), decodedW, decodedH,
                                 QImage::Format_RGB32);
                    emit imageReady(frame.copy());
                    frameOk = true;
                }
            } else {
                VLOG_DEBUG(MODULE, "MJPEG MPP decode failed, skipping frame");
            }
        } else {
            VLOG_WARN(MODULE, "unsupported pixel format 0x%08x, skipping frame",
                      m_pixelFormat);
        }
    }

    if (ioctl(m_fd, VIDIOC_QBUF, &buf) < 0) {
        VLOG_ERROR(MODULE, "VIDIOC_QBUF recycle failed: %s", strerror(errno));
        return false;
    }

    return frameOk;
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// YUV → RGB conversion (CPU fallback for NV21/UYVY)
//
// NV12 is handled by rga::nv12ToRgb32() (RGA hardware or built-in CPU fallback).
// These use integer-only arithmetic for embedded performance.
// ---------------------------------------------------------------------------

QImage CaptureThread::nv21ToRgb(const uint8_t *data, int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    const uint8_t *yPlane = data;
    const uint8_t *vuPlane = data + width * height;

    for (int y = 0; y < height; y++) {
        QRgb *scanLine = reinterpret_cast<QRgb *>(image.scanLine(y));
        const uint8_t *yRow = yPlane + y * width;
        const uint8_t *vuRow = vuPlane + (y / 2) * width;

        for (int x = 0; x < width; x++) {
            int Y = yRow[x];
            int V = vuRow[(x / 2) * 2] - 128;
            int U = vuRow[(x / 2) * 2 + 1] - 128;

            int R = Y + ((359 * V) >> 8);
            int G = Y - ((88 * U + 183 * V) >> 8);
            int B = Y + ((454 * U) >> 8);

            scanLine[x] = qRgb(qBound(0, R, 255),
                               qBound(0, G, 255),
                               qBound(0, B, 255));
        }
    }
    return image;
}

QImage CaptureThread::uyvyToRgb(const uint8_t *data, int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);

    for (int y = 0; y < height; y++) {
        QRgb *scanLine = reinterpret_cast<QRgb *>(image.scanLine(y));
        const uint8_t *row = data + y * width * 2;

        for (int x = 0; x < width; x += 2) {
            int U  = row[x * 2 + 0] - 128;
            int Y0 = row[x * 2 + 1];
            int V  = row[x * 2 + 2] - 128;
            int Y1 = row[x * 2 + 3];

            int R0 = Y0 + ((359 * V) >> 8);
            int G0 = Y0 - ((88 * U + 183 * V) >> 8);
            int B0 = Y0 + ((454 * U) >> 8);

            int R1 = Y1 + ((359 * V) >> 8);
            int G1 = Y1 - ((88 * U + 183 * V) >> 8);
            int B1 = Y1 + ((454 * U) >> 8);

            scanLine[x] = qRgb(qBound(0, R0, 255),
                               qBound(0, G0, 255),
                               qBound(0, B0, 255));
            if (x + 1 < width) {
                scanLine[x + 1] = qRgb(qBound(0, R1, 255),
                                       qBound(0, G1, 255),
                                       qBound(0, B1, 255));
            }
        }
    }
    return image;
}

QImage CaptureThread::yuyvToRgb(const uint8_t *data, int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);

    for (int y = 0; y < height; y++) {
        QRgb *scanLine = reinterpret_cast<QRgb *>(image.scanLine(y));
        const uint8_t *row = data + y * width * 2;

        for (int x = 0; x < width; x += 2) {
            int Y0 = row[x * 2 + 0];
            int U  = row[x * 2 + 1] - 128;
            int Y1 = row[x * 2 + 2];
            int V  = row[x * 2 + 3] - 128;

            int R0 = Y0 + ((359 * V) >> 8);
            int G0 = Y0 - ((88 * U + 183 * V) >> 8);
            int B0 = Y0 + ((454 * U) >> 8);

            int R1 = Y1 + ((359 * V) >> 8);
            int G1 = Y1 - ((88 * U + 183 * V) >> 8);
            int B1 = Y1 + ((454 * U) >> 8);

            scanLine[x] = qRgb(qBound(0, R0, 255),
                               qBound(0, G0, 255),
                               qBound(0, B0, 255));
            if (x + 1 < width) {
                scanLine[x + 1] = qRgb(qBound(0, R1, 255),
                                       qBound(0, G1, 255),
                                       qBound(0, B1, 255));
            }
        }
    }
    return image;
}
