#ifndef AP3216_IIOBACKEND_H
#define AP3216_IIOBACKEND_H

#include "ap3216device.h"

#include <string>

namespace vehicledevice {

class Ap3216IioBackend final : public IAp3216Backend
{
public:
    explicit Ap3216IioBackend(const Ap3216Config &config);
    ~Ap3216IioBackend() override;

    bool open(std::string *errorMessage) override;
    void close() override;
    bool isOpen() const override;
    Ap3216Reading readSensorValues(std::string *errorMessage) override;
    const char *backendName() const override;
    int eventFileDescriptor() const override;
    int readEvent(Ap3216Event *event,
                  std::string *errorMessage) override;

private:
    bool resolveSysfsPath(std::string *errorMessage);
    bool validateSysfsPath(const std::string &path,
                           std::string *errorMessage) const;
    bool enableEvents(std::string *errorMessage);
    void disableEvents();

    Ap3216Config config_;
    std::string sysfsPath_;
    std::string eventDevicePath_;
    int eventFd_;
    bool open_;
    bool eventsEnabled_;
};

} // namespace vehicledevice

#endif // AP3216_IIOBACKEND_H
