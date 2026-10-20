#ifndef CAPTURE_THREAD_H
#define CAPTURE_THREAD_H

#include <QThread>
#include <QImage>
#include <QMutex>
#include <QWaitCondition>
#include <cstdint>
#include <vector>
#include <string>

#include "rknn_detector.h"
#include "v4l2_frame_telemetry.h"

namespace vehiclecamera {
struct CameraConfig;
}

// V4L2 multiplanar capture module for RK3588S rkisp ISP.
// Lifecycle: init() → startCapture() → stopCapture() → shutdown()
//
// Thread safety: all public methods are safe to call from any thread.
// State transitions are serialized through m_stateMutex.
// Frame delivery emits imageReady(QImage) on the internal QThread.

class CaptureThread : public QThread
{
    Q_OBJECT

public:
    // Capture module lifecycle state.
    enum class State {
        Uninitialized,  // init() not called yet
        Ready,          // configured, not capturing
        Starting,       // transitioning to Capturing
        Capturing,      // actively delivering frames
        Stopping,       // transitioning to Ready
        Error           // last operation failed, call shutdown() to reset
    };
    static const char *stateToString(State state);

    // Error codes describing why a lifecycle transition failed.
    enum class ErrorCode {
        None = 0,
        DeviceOpenFailed,
        FormatNotSupported,
        BufferSetupFailed,
        StreamStartFailed,
        FrameCaptureError
    };
    static const char *errorCodeToString(ErrorCode code);

    explicit CaptureThread(QObject *parent = nullptr);
    ~CaptureThread() override;

    // Lifecycle.

    // Configure capture parameters. Transitions from Uninitialized to Ready.
    // Returns true if config is valid and module is ready.
    bool init(const vehiclecamera::CameraConfig &config);

    // Begin capturing frames. Emits imageReady(QImage) for each frame.
    // Transitions from Ready to Capturing (via Starting).
    // Returns true if capture thread started successfully.
    bool startCapture();

    // Stop capturing. Transitions from Capturing to Ready (via Stopping).
    // Blocks until the capture thread exits.
    void stopCapture();

    // Release all resources (buffers, fd, thread). Transitions to Uninitialized.
    void shutdown();

    // Query current state.
    State state() const;
    ErrorCode lastError() const;
    std::string lastErrorMessage() const;
    bool isCapturing() const;

signals:
    // Emitted for each captured frame. QImage is a deep copy, safe to use.
    void imageReady(QImage frame);

    // Emitted when capture state changes.
    void stateChanged(State newState);

    // Emitted when a non-fatal frame error occurs during capture loop.
    void frameError(QString description);

private:
    void run() override;

    bool openDevice();
    bool negotiateFormat();
    bool setupBuffers();
    bool startStream();
    void stopStream();
    void releaseBuffers();
    void closeDevice();

    void transitionTo(State newState);
    void transitionToError(ErrorCode code, const std::string &message);

    bool dequeueAndEmitFrame();

    static std::uint64_t monotonicNowNs();
    void resetFrameTelemetry();
    void observeFrameTelemetry(std::uint32_t sequence);
    void logFrameTelemetry(
        const char *reason,
        std::uint64_t nowMonotonicNs);

    // YUV to RGB conversion (CPU fallback for NV21/UYVY/YUYV).
    // NV12 is handled by rga::nv12ToRgb32() in rga_converter.h.
    QImage nv21ToRgb(const uint8_t *data, int width, int height);
    QImage uyvyToRgb(const uint8_t *data, int width, int height);
    QImage yuyvToRgb(const uint8_t *data, int width, int height);

    struct BufferSlot {
        void *start;
        unsigned int length;
    };

    // Configuration (set once in init).
    vehiclecamera::CameraConfig *m_config;

    // V4L2 state.
    int m_fd;
    uint32_t m_pixelFormat;
    uint32_t m_bufferType;
    bool m_multiplanarCapture;
    int m_actualWidth;
    int m_actualHeight;
    std::vector<BufferSlot> m_buffers;
    std::vector<uint8_t> m_rgbBuffer;    // Pre-allocated buffer for RGA/CPU NV12→RGB32 output.
    std::vector<uint8_t> m_nv12Buffer;   // Pre-allocated buffer for MPP MJPEG→NV12 output.

    // V4L2 dequeue telemetry. Accessed only by the capture worker thread.
    vehiclecamera::V4L2FrameTelemetry m_frameTelemetry;
    std::uint64_t m_lastTelemetryLogMonotonicNs;

    // Lifecycle state (protected by m_stateMutex).
    mutable QMutex m_stateMutex;
    State m_state;
    ErrorCode m_lastError;
    std::string m_lastErrorMessage;

    // Thread control.
    QMutex m_stopMutex;
    QWaitCondition m_startCondition;  // signals start→Capturing or start→Error
    bool m_stopRequested;
    bool m_threadExited;             // set by run() before exit
};

#endif // CAPTURE_THREAD_H
