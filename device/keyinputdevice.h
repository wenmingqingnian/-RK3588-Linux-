#ifndef KEYINPUTDEVICE_H
#define KEYINPUTDEVICE_H

#include "vehicledeviceconfig.h"

#include <string>

namespace vehicledevice {

class KeyInputDevice {
public:
    KeyInputDevice();
    explicit KeyInputDevice(const KeyInputConfig &config);
    ~KeyInputDevice();

    KeyInputDevice(const KeyInputDevice &) = delete;
    KeyInputDevice &operator=(const KeyInputDevice &) = delete;

    void configure(const KeyInputConfig &config);
    bool openDevice(std::string *errorMessage);
    int readKeyValue(std::string *errorMessage);
    bool isAvailable() const;

private:
    void closeDevice();

    KeyInputConfig config_;
    int fd_;
};

} // namespace vehicledevice

#endif // KEYINPUTDEVICE_H
