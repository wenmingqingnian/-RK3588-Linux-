#ifndef VEHICLEDEVICE_GPSDEVICE_H
#define VEHICLEDEVICE_GPSDEVICE_H

// Real GPS serial device layer for RK3588S.
//
// Owns a UART file descriptor configured via termios. Provides open/poll/read/
// close with timeout, error reporting and repeatable close. The device itself
// owns no threads; the GPS worker layer (GpsWorker) drives the read loop and
// emits Qt signals to the GUI.
//
// Thread/lock rules (Phase 9):
//   - std::mutex is NOT reentrant.
//   - closeDevice() (public) acquires mutex_ and calls closeDeviceLocked().
//   - closeDeviceLocked() (private) assumes the caller holds mutex_.
//   - readChunk() acquires mutex_ for the poll+read cycle. The poll timeout is
//     bounded by config_.readTimeoutMs so a stop request from another thread
//     never waits longer than that for the lock.
//   - The worker must set its running flag, join its thread, THEN call
//     closeDevice(). Calling closeDevice() while readChunk() is mid-poll would
//     block until the poll timeout elapses.
//
// Platform guards: termios/poll/serial APIs are Linux-only. On other platforms
// openDevice fails with an explicit error and all other calls are no-ops.

#include "vehicledeviceconfig.h"

#include <cstddef>
#include <mutex>
#include <string>

namespace vehicledevice {

class GpsDevice {
public:
    explicit GpsDevice(const GpsSerialConfig &config);
    ~GpsDevice();

    // Open the serial port and apply termios configuration.
    // Returns true on success (or when disabled — no-op success).
    // Returns false and sets errorMessage on failure.
    bool openDevice(std::string *errorMessage);

    // Release the serial port fd. Safe to call multiple times.
    void closeDevice();

    // Whether openDevice() succeeded and the fd is valid.
    bool isAvailable() const;

    // Read one chunk of bytes from the serial port using poll() with the
    // configured timeout. Returns the number of bytes read (>=0), or -1 on
    // error. A timeout is NOT an error: returns 0 with outError cleared.
    //   >0: bytes read into buffer
    //    0: timeout, no data available
    //   -1: error (fd not open, poll error, read error) — outError set
    int readChunk(char *buffer, std::size_t bufferSize, std::string *outError);

private:
    GpsSerialConfig config_;
    int fd_;
    mutable std::mutex mutex_;

    // Internal cleanup without locking. Caller must hold mutex_.
    void closeDeviceLocked();

    GpsDevice(const GpsDevice &);
    GpsDevice &operator=(const GpsDevice &);
};

} // namespace vehicledevice

#endif // VEHICLEDEVICE_GPSDEVICE_H
