#ifndef AP3216_USERI2CBACKEND_H
#define AP3216_USERI2CBACKEND_H

#include "ap3216device.h"

#include <cstdint>
#include <string>

namespace vehicledevice {

class Ap3216UserI2cBackend final : public IAp3216Backend
{
public:
    explicit Ap3216UserI2cBackend(const Ap3216Config &config);
    ~Ap3216UserI2cBackend() override;

    bool open(std::string *errorMessage) override;
    void close() override;
    bool isOpen() const override;
    Ap3216Reading readSensorValues(std::string *errorMessage) override;
    const char *backendName() const override;
    int eventFileDescriptor() const override;
    int readEvent(Ap3216Event *event,
                  std::string *errorMessage) override;

private:
    bool readRegister(std::uint8_t reg,
                      std::uint8_t *value,
                      std::string *errorMessage) const;
    bool writeRegister(std::uint8_t reg,
                       std::uint8_t value,
                       std::string *errorMessage) const;
    bool readRegisterPair(std::uint8_t lowReg,
                          std::uint8_t highReg,
                          std::uint8_t *low,
                          std::uint8_t *high,
                          std::string *errorMessage) const;
    bool checkResetDefaults(std::string *errorMessage) const;
    bool programThreshold(std::uint8_t lowReg,
                          std::uint8_t highReg,
                          int threshold,
                          std::string *errorMessage) const;
    bool initializeHardware(std::string *errorMessage) const;

    Ap3216Config config_;
    int fd_;
    bool open_;
};

} // namespace vehicledevice

#endif // AP3216_USERI2CBACKEND_H
