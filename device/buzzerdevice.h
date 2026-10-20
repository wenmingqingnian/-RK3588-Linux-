#ifndef BUZZERDEVICE_H
#define BUZZERDEVICE_H

#include "vehicledeviceconfig.h"

#include <mutex>
#include <string>

namespace vehicledevice {

// Map alarm state to physical GPIO output level (0 or 1).
//
// activeLow=true (low-level trigger):
//   active=true  -> 0 (beep)
//   active=false -> 1 (silent)
//
// activeLow=false (high-level trigger):
//   active=true  -> 1 (beep)
//   active=false -> 0 (silent)
//
// Pure function for unit testing without /dev/gpiochip access.
// Returns 0 or 1.
int buzzerAlarmToPhysicalLevel(bool activeLow, bool active);

class BuzzerDevice {
public:
    explicit BuzzerDevice(const BuzzerConfig &config);
    ~BuzzerDevice();

    // Open the gpiochip and request the GPIO line handle.
    // Returns true on success (or when disabled — no-op success).
    // Returns false and sets errorMessage on failure.
    bool openDevice(std::string *errorMessage);

    // Release the GPIO line handle and close the gpiochip fd.
    // Safe to call multiple times.
    void closeDevice();

    // Whether openDevice() succeeded and the line handle is active.
    bool isAvailable() const;

    // Set the alarm on/off by writing the physical level to the GPIO line.
    // Thread-safe. Returns true on success, false and sets errorMessage on
    // failure (e.g. device not open, ioctl error).
    bool setAlarm(bool active, std::string *errorMessage);

private:
    BuzzerConfig config_;
    int chipFd_;
    int lineHandleFd_;
    mutable std::mutex mutex_;

    // Internal cleanup without locking. Caller must hold mutex_.
    void closeDeviceLocked();

    BuzzerDevice(const BuzzerDevice &);
    BuzzerDevice &operator=(const BuzzerDevice &);
};

} // namespace vehicledevice

#endif // BUZZERDEVICE_H
