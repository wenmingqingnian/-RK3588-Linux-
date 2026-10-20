#include "ap3216_useri2cbackend.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

namespace vehicledevice {
namespace {

// Register map and data layout are kept identical to the frozen kernel driver
// drivers/iio/light/ap3216c.[ch] at kernel commit c0433ef265af.
constexpr std::uint8_t kRegSysConf = 0x00;
constexpr std::uint8_t kRegIntClearMode = 0x02;
constexpr std::uint8_t kRegIrDataLow = 0x0a;
constexpr std::uint8_t kRegIrDataHigh = 0x0b;
constexpr std::uint8_t kRegAlsDataLow = 0x0c;
constexpr std::uint8_t kRegAlsDataHigh = 0x0d;
constexpr std::uint8_t kRegPsDataLow = 0x0e;
constexpr std::uint8_t kRegPsDataHigh = 0x0f;
constexpr std::uint8_t kRegAlsCalibration = 0x19;
constexpr std::uint8_t kRegPsConfig = 0x20;
constexpr std::uint8_t kRegPsLedControl = 0x21;
constexpr std::uint8_t kRegPsIntForm = 0x22;
constexpr std::uint8_t kRegPsLowThresholdLow = 0x2a;
constexpr std::uint8_t kRegPsLowThresholdHigh = 0x2b;
constexpr std::uint8_t kRegPsHighThresholdLow = 0x2c;
constexpr std::uint8_t kRegPsHighThresholdHigh = 0x2d;

constexpr std::uint8_t kModePowerDown = 0x00;
constexpr std::uint8_t kModeAlsPsIr = 0x03;
constexpr std::uint8_t kModeSoftwareReset = 0x04;
constexpr std::uint8_t kInterruptClearByDataRead = 0x00;
constexpr std::uint8_t kPsInterruptHysteresis = 0x01;

constexpr std::uint8_t kDefaultAlsCalibration = 0x40;
constexpr std::uint8_t kDefaultPsConfig = 0x05;
constexpr std::uint8_t kDefaultPsLedControl = 0x13;

constexpr std::uint8_t kPsIrOverflow = 0x40;
constexpr std::uint8_t kIrOverflow = 0x80;
constexpr std::uint8_t kPsLowMask = 0x0f;
constexpr std::uint8_t kPsHighMask = 0x3f;
constexpr std::uint8_t kIrLowMask = 0x03;
constexpr std::uint8_t kThresholdLowMask = 0x03;

void setError(std::string *errorMessage, const std::string &message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

std::string errnoMessage(const std::string &operation)
{
    return operation + ": " + std::strerror(errno);
}

} // namespace

Ap3216UserI2cBackend::Ap3216UserI2cBackend(const Ap3216Config &config)
    : config_(config),
      fd_(-1),
      open_(false)
{
}

Ap3216UserI2cBackend::~Ap3216UserI2cBackend()
{
    close();
}

bool Ap3216UserI2cBackend::readRegister(
    std::uint8_t reg,
    std::uint8_t *value,
    std::string *errorMessage) const
{
    if (fd_ < 0 || value == nullptr) {
        setError(errorMessage, "ap3216 user_i2c invalid read state");
        return false;
    }

    i2c_msg messages[2]{};
    messages[0].addr = static_cast<__u16>(config_.i2cAddress);
    messages[0].flags = 0;
    messages[0].len = 1;
    messages[0].buf = &reg;
    messages[1].addr = static_cast<__u16>(config_.i2cAddress);
    messages[1].flags = I2C_M_RD;
    messages[1].len = 1;
    messages[1].buf = value;

    i2c_rdwr_ioctl_data transfer{};
    transfer.msgs = messages;
    transfer.nmsgs = 2;

    if (::ioctl(fd_, I2C_RDWR, &transfer) < 0) {
        setError(errorMessage,
                 errnoMessage("ap3216 user_i2c register read failed"));
        return false;
    }
    return true;
}

bool Ap3216UserI2cBackend::writeRegister(
    std::uint8_t reg,
    std::uint8_t value,
    std::string *errorMessage) const
{
    if (fd_ < 0) {
        setError(errorMessage, "ap3216 user_i2c invalid write state");
        return false;
    }

    std::uint8_t payload[2] = {reg, value};
    i2c_msg message{};
    message.addr = static_cast<__u16>(config_.i2cAddress);
    message.flags = 0;
    message.len = sizeof(payload);
    message.buf = payload;

    i2c_rdwr_ioctl_data transfer{};
    transfer.msgs = &message;
    transfer.nmsgs = 1;

    if (::ioctl(fd_, I2C_RDWR, &transfer) < 0) {
        setError(errorMessage,
                 errnoMessage("ap3216 user_i2c register write failed"));
        return false;
    }
    return true;
}

bool Ap3216UserI2cBackend::readRegisterPair(
    std::uint8_t lowReg,
    std::uint8_t highReg,
    std::uint8_t *low,
    std::uint8_t *high,
    std::string *errorMessage) const
{
    return readRegister(lowReg, low, errorMessage) &&
           readRegister(highReg, high, errorMessage);
}

bool Ap3216UserI2cBackend::checkResetDefaults(
    std::string *errorMessage) const
{
    std::uint8_t value = 0;
    if (!readRegister(kRegSysConf, &value, errorMessage) ||
        value != kModePowerDown) {
        if (errorMessage != nullptr && errorMessage->empty()) {
            *errorMessage = "ap3216 unexpected SYS_CONF reset value";
        }
        return false;
    }
    if (!readRegister(kRegAlsCalibration, &value, errorMessage) ||
        value != kDefaultAlsCalibration) {
        if (errorMessage != nullptr && errorMessage->empty()) {
            *errorMessage = "ap3216 unexpected ALS calibration default";
        }
        return false;
    }
    if (!readRegister(kRegPsConfig, &value, errorMessage) ||
        value != kDefaultPsConfig) {
        if (errorMessage != nullptr && errorMessage->empty()) {
            *errorMessage = "ap3216 unexpected PS configuration default";
        }
        return false;
    }
    if (!readRegister(kRegPsLedControl, &value, errorMessage) ||
        value != kDefaultPsLedControl) {
        if (errorMessage != nullptr && errorMessage->empty()) {
            *errorMessage = "ap3216 unexpected PS LED-control default";
        }
        return false;
    }
    return true;
}

bool Ap3216UserI2cBackend::programThreshold(
    std::uint8_t lowReg,
    std::uint8_t highReg,
    int threshold,
    std::string *errorMessage) const
{
    return writeRegister(lowReg,
                         static_cast<std::uint8_t>(threshold) &
                             kThresholdLowMask,
                         errorMessage) &&
           writeRegister(highReg,
                         static_cast<std::uint8_t>(threshold >> 2),
                         errorMessage);
}

bool Ap3216UserI2cBackend::initializeHardware(
    std::string *errorMessage) const
{
    if (!writeRegister(kRegSysConf, kModeSoftwareReset, errorMessage)) {
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    if (!checkResetDefaults(errorMessage) ||
        !writeRegister(kRegIntClearMode,
                       kInterruptClearByDataRead, errorMessage) ||
        !writeRegister(kRegPsIntForm,
                       kPsInterruptHysteresis, errorMessage) ||
        !programThreshold(kRegPsLowThresholdLow,
                          kRegPsLowThresholdHigh,
                          config_.psFallingThreshold,
                          errorMessage) ||
        !programThreshold(kRegPsHighThresholdLow,
                          kRegPsHighThresholdHigh,
                          config_.psRisingThreshold,
                          errorMessage) ||
        !writeRegister(kRegSysConf, kModeAlsPsIr, errorMessage)) {
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return true;
}

bool Ap3216UserI2cBackend::open(std::string *errorMessage)
{
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    close();
    fd_ = ::open(config_.i2cDevice.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        setError(errorMessage,
                 errnoMessage("ap3216 cannot open " + config_.i2cDevice));
        return false;
    }

    if (::ioctl(fd_, I2C_SLAVE, config_.i2cAddress) < 0) {
        const std::string detail =
            errnoMessage("ap3216 cannot claim I2C address 0x1e");
        ::close(fd_);
        fd_ = -1;
        setError(errorMessage,
                 detail +
                     "; unbind the kernel ap3216c driver before user_i2c mode");
        return false;
    }

    if (!initializeHardware(errorMessage)) {
        close();
        return false;
    }

    open_ = true;
    return true;
}

void Ap3216UserI2cBackend::close()
{
    if (fd_ >= 0) {
        std::string ignored;
        writeRegister(kRegSysConf, kModePowerDown, &ignored);
        ::close(fd_);
        fd_ = -1;
    }
    open_ = false;
}

bool Ap3216UserI2cBackend::isOpen() const
{
    return open_;
}

Ap3216Reading Ap3216UserI2cBackend::readSensorValues(
    std::string *errorMessage)
{
    Ap3216Reading reading;
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    if (!open_) {
        reading.errorMessage = "ap3216 user_i2c backend is not open";
        setError(errorMessage, reading.errorMessage);
        return reading;
    }

    std::uint8_t low = 0;
    std::uint8_t high = 0;
    if (!readRegisterPair(kRegAlsDataLow, kRegAlsDataHigh,
                          &low, &high, &reading.errorMessage)) {
        setError(errorMessage, reading.errorMessage);
        return reading;
    }
    reading.als = static_cast<int>(low) |
                  (static_cast<int>(high) << 8);

    if (!readRegisterPair(kRegPsDataLow, kRegPsDataHigh,
                          &low, &high, &reading.errorMessage)) {
        setError(errorMessage, reading.errorMessage);
        return reading;
    }
    if ((low | high) & kPsIrOverflow) {
        reading.errorMessage = "ap3216 proximity sample overflow";
        setError(errorMessage, reading.errorMessage);
        return reading;
    }
    reading.ps = static_cast<int>(low & kPsLowMask) |
                 (static_cast<int>(high & kPsHighMask) << 4);

    if (!readRegisterPair(kRegIrDataLow, kRegIrDataHigh,
                          &low, &high, &reading.errorMessage)) {
        setError(errorMessage, reading.errorMessage);
        return reading;
    }
    if (low & kIrOverflow) {
        reading.errorMessage = "ap3216 infrared sample overflow";
        setError(errorMessage, reading.errorMessage);
        return reading;
    }
    reading.ir = static_cast<int>(low & kIrLowMask) |
                 (static_cast<int>(high) << 2);

    reading.valid = true;
    return reading;
}

const char *Ap3216UserI2cBackend::backendName() const
{
    return "user_i2c";
}

int Ap3216UserI2cBackend::eventFileDescriptor() const
{
    return -1;
}

int Ap3216UserI2cBackend::readEvent(Ap3216Event *event,
                                    std::string *errorMessage)
{
    if (event != nullptr) {
        *event = Ap3216Event();
    }
    setError(errorMessage,
             "ap3216 user_i2c backend does not expose IIO events");
    return -1;
}

} // namespace vehicledevice
