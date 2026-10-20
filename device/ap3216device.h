#ifndef AP3216DEVICE_H
#define AP3216DEVICE_H

#include "vehicledeviceconfig.h"

#include <cstdint>
#include <memory>
#include <string>

namespace vehicledevice {

enum class Ap3216EventDirection {
    Rising,
    Falling
};

struct Ap3216Reading {
    bool valid = false;
    int als = 0;
    int ps = 0;
    int ir = 0;
    std::string errorMessage;
};

struct Ap3216Event {
    bool valid = false;
    Ap3216EventDirection direction = Ap3216EventDirection::Rising;
    std::int64_t timestampNs = 0;
    std::string errorMessage;
};

// AP3216C application data-source abstraction.
//
// The IIO backend owns only the IIO sysfs/event handles. The user-I2C backend
// owns only /dev/i2c-N. A process selects exactly one backend at startup; the
// two implementations must never access address 0x1e concurrently.
class IAp3216Backend
{
public:
    virtual ~IAp3216Backend() = default;

    virtual bool open(std::string *errorMessage) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual Ap3216Reading readSensorValues(std::string *errorMessage) = 0;
    virtual const char *backendName() const = 0;

    // IIO backend: returns the event fd obtained with
    // IIO_GET_EVENT_FD_IOCTL from /dev/iio:deviceX.
    // User-I2C backend: returns -1 and uses polling hysteresis in the worker.
    virtual int eventFileDescriptor() const = 0;

    // Return 1 when one event was read, 0 for EAGAIN/no event, -1 on error.
    virtual int readEvent(Ap3216Event *event,
                          std::string *errorMessage) = 0;
};

std::unique_ptr<IAp3216Backend>
createAp3216Backend(const Ap3216Config &config);

} // namespace vehicledevice

#endif // AP3216DEVICE_H
