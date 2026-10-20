#ifndef VEHICLEDEVICE_GPSWORKER_H
#define VEHICLEDEVICE_GPSWORKER_H

// GPS worker thread bridge between the serial device layer and the Qt GUI.
//
// Owns a std::thread that drives either:
//   - real serial GPS (GpsDevice + NmeaStreamBuffer + parseNmeaSentence)
//   - simulation playback (GpsSimulator, which reuses the same parser)
//
// The thread emits Qt signals to the GUI. The GUI never calls into the device
// or parser directly. Cross-thread signal/slot connections are queued, so the
// GpsFix type is registered with Q_DECLARE_METATYPE / qRegisterMetaType.
//
// Stop sequence (Phase 9 point 6):
//   1. Lock mutex_, set running_=false, release mutex_.
//   2. The poll-based readChunk() or simulation sleep returns within a bounded
//      interval (≤ readTimeoutMs or ≤ sleep step), the loop checks running_,
//      exits, and the thread function returns.
//   3. Join the thread (no lock held).
//   4. Close the device fd (no lock held — thread already joined).
//
// The worker never holds mutex_ while joining the thread and never calls a
// mutex-locking public method from another mutex-locking method.

#include "gpsdevice.h"
#include "gpssimulator.h"
#include "nmeaparser.h"
#include "vehicledeviceconfig.h"

#include <QObject>
#include <QString>

#include <thread>

// Make vehiclegps::GpsFix usable in queued signal/slot connections.
Q_DECLARE_METATYPE(vehiclegps::GpsFix)

class GpsWorker : public QObject
{
    Q_OBJECT

public:
    explicit GpsWorker(const vehicledevice::GpsSerialConfig &serialConfig,
                       const vehicledevice::GpsSimulationConfig &simulationConfig,
                       QObject *parent = nullptr);
    ~GpsWorker();

public slots:
    // Launch the worker thread. Picks real-GPS or simulation mode based on
    // config. Safe to call once; repeated calls while running are ignored.
    void start();

    // Signal the worker thread to stop, join it, and release the device.
    // Safe to call multiple times.
    void stop();

signals:
    // Emitted on every valid fix (real or simulated). `simulated` is true
    // for simulation-mode fixes so the GUI can label them accordingly.
    void fixUpdated(vehiclegps::GpsFix fix);

    // Emitted when the device/simulator becomes available (true) or
    // unavailable (false, e.g. after stop or open failure).
    void availabilityChanged(bool available);

    // Human-readable status for the GUI. One of:
    //   "GPS已关闭"       — both real and simulation disabled
    //   "GPS设备不可用"   — real device open failed
    //   "GPS未定位"       — device open, waiting for first valid fix
    //   "GPS已定位"       — valid fix received from real device
    //   "GPS模拟定位"     — simulation mode active
    //   "GPS数据异常"     — simulator file load failed or data error
    void statusChanged(QString statusText);

    // Non-fatal warnings (read errors, parse failures). The worker continues.
    void warning(QString message);

private:
    vehicledevice::GpsSerialConfig serialConfig_;
    vehicledevice::GpsSimulationConfig simulationConfig_;
    vehicledevice::GpsDevice device_;
    vehiclegps::GpsSimulator simulator_;

    std::thread thread_;
    mutable std::mutex mutex_;
    bool running_;

    // Read running_ under lock. Returns a snapshot.
    bool isRunning() const;

    // Set running_ under lock.
    void setRunning(bool value);

    // Thread entry points.
    void runRealGpsLoop();
    void runSimulationLoop();

    GpsWorker(const GpsWorker &);
    GpsWorker &operator=(const GpsWorker &);
};

#endif // VEHICLEDEVICE_GPSWORKER_H
