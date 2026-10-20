#include "mpu6050device.h"

#include "smbus.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

// Windows 主机构建兼容：MinGW730 的 fcntl.h 不定义 O_CLOEXEC。
// Linux 上 O_CLOEXEC 是 POSIX.1-2008 标准，定义正常。
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

namespace vehicledevice {
namespace {

const char *const MODULE = "mpu6050";

void setError(std::string *errorMessage, const std::string &message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

// Combines a prefix with errno string for I2C error reporting.
std::string i2cError(const std::string &prefix)
{
    return prefix + ": " + std::strerror(errno);
}

// MPU6050 sensitivity (LSB per unit) constants from datasheet §3.2/§3.4.
//   ±2g:  16384 LSB/g
//   ±4g:   8192 LSB/g
//   ±8g:   4096 LSB/g
//   ±16g:  2048 LSB/g
//   ±250 dps:  131 LSB/dps
//   ±500 dps:   65.5 LSB/dps
//   ±1000 dps:  32.8 LSB/dps
//   ±2000 dps:  16.4 LSB/dps
// Gravity g = 9.80665 m/s^2 (standard, used throughout).

constexpr double kGravity = 9.80665;

// Returns LSB-per-g for the given ±range. Returns 0 for invalid range.
double accelSensitivity(int rangeG)
{
    switch (rangeG) {
    case 2:  return 16384.0;
    case 4:  return 8192.0;
    case 8:  return 4096.0;
    case 16: return 2048.0;
    default: return 0.0;
    }
}

// Returns LSB-per-dps for the given ±range. Returns 0 for invalid range.
double gyroSensitivity(int rangeDps)
{
    switch (rangeDps) {
    case 250:  return 131.0;
    case 500:  return 65.5;
    case 1000: return 32.8;
    case 2000: return 16.4;
    default:   return 0.0;
    }
}

// DLPF_CFG -> bandwidth (Hz) mapping from datasheet §8.3.4 REG 26 CONFIG.
//   0: 260 Hz   1: 188   2: 94   3: 44   4: 21   5: 10   6: 5   7: reserved
// We accept the reverse mapping here.
[[maybe_unused]] int dlpfCfgToBandwidth(int cfg)
{
    static const int table[] = {260, 188, 94, 44, 21, 10, 5, -1};
    if (cfg < 0 || cfg > 6) {
        return -1;
    }
    return table[cfg];
}

} // namespace

Mpu6050Reading::Mpu6050Reading()
    : valid(false),
      accelX(0.0),
      accelY(0.0),
      accelZ(0.0),
      gyroX(0.0),
      gyroY(0.0),
      gyroZ(0.0),
      temperature(0.0),
      errorMessage()
{
}

Mpu6050Device::Mpu6050Device(const Mpu6050Config &config)
    : accelScale_(0.0),
      gyroScale_(0.0),
      config_(config),
      fd_(-1),
      available_(false)
{
}

Mpu6050Device::~Mpu6050Device()
{
    closeDevice();
}

int Mpu6050Device::dlpfBandwidthToCfg(int bandwidthHz)
{
    switch (bandwidthHz) {
    case 260: return 0;
    case 188: return 1;
    case 94:  return 2;
    case 44:  return 3;
    case 21:  return 4;
    case 10:  return 5;
    case 5:   return 6;
    default:  return -1;
    }
}

uint8_t Mpu6050Device::gyroRangeToConfig(int rangeDps)
{
    switch (rangeDps) {
    case 250:  return 0x00;
    case 500:  return 0x08;
    case 1000: return 0x10;
    case 2000: return 0x18;
    default:   return 0x08;  // safe default ±500 dps
    }
}

uint8_t Mpu6050Device::accelRangeToConfig(int rangeG)
{
    switch (rangeG) {
    case 2:  return 0x00;
    case 4:  return 0x08;
    case 8:  return 0x10;
    case 16: return 0x18;
    default: return 0x08;  // safe default ±4g
    }
}

int Mpu6050Device::readByte(uint8_t reg, std::string *errorMessage) const
{
    const __s32 value = i2c_smbus_read_byte_data(fd_, reg);
    if (value < 0) {
        setError(errorMessage, i2cError("mpu6050 read_byte reg=0x" +
                                       std::to_string(reg)));
        return -1;
    }
    return static_cast<int>(value);
}

bool Mpu6050Device::writeByte(uint8_t reg, uint8_t value,
                              std::string *errorMessage) const
{
    const __s32 rc = i2c_smbus_write_byte_data(fd_, reg, value);
    if (rc < 0) {
        setError(errorMessage, i2cError("mpu6050 write_byte reg=0x" +
                                       std::to_string(reg)));
        return false;
    }
    return true;
}

bool Mpu6050Device::readBlock(uint8_t reg, uint8_t *buffer, uint8_t length,
                              std::string *errorMessage) const
{
    const __s32 count = i2c_smbus_read_i2c_block_data(fd_, reg, length, buffer);
    if (count < 0) {
        setError(errorMessage, i2cError("mpu6050 read_block reg=0x" +
                                       std::to_string(reg)));
        return false;
    }
    if (count != length) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "mpu6050 short read: expected %d, got %d",
                      static_cast<int>(length), static_cast<int>(count));
        setError(errorMessage, buf);
        return false;
    }
    return true;
}

bool Mpu6050Device::openDevice(std::string *errorMessage)
{
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    closeDevice();

    if (!config_.enabled) {
        return true;
    }

    if (config_.i2cDevice.empty()) {
        setError(errorMessage, "mpu6050 i2c_device is empty");
        return false;
    }

    fd_ = ::open(config_.i2cDevice.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        setError(errorMessage, i2cError("mpu6050 open " + config_.i2cDevice));
        return false;
    }

    // Set I2C_SLAVE address (ioctl, not SMBus). Force flag is not needed
    // because 0x68 is the standard MPU6050 address.
    if (::ioctl(fd_, I2C_SLAVE, config_.i2cAddress) < 0) {
        setError(errorMessage, i2cError("mpu6050 ioctl I2C_SLAVE addr=0x" +
                                       std::to_string(config_.i2cAddress)));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Step 1: verify device identity (WHO_AM_I == 0x68 for MPU6050,
    // 0x70 for MPU9250 legacy). Reject clones / wrong addresses.
    std::string whoAmIError;
    const int whoAmI = readByte(kWhoAmIRegister, &whoAmIError);
    if (whoAmI < 0) {
        setError(errorMessage, "mpu6050 WHO_AM_I read failed: " + whoAmIError);
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    if (whoAmI != kWhoAmIExpectedValue && whoAmI != kWhoAmIAcceptedAltValue) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "mpu6050 WHO_AM_I mismatch: expected 0x%02X, got 0x%02X",
                      kWhoAmIExpectedValue, whoAmI);
        setError(errorMessage, buf);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Step 2: wake device from sleep (PWR_MGMT_1 bit 6 = 0).
    // Also set clock source to PLL with Z-axis gyro reference (0x01)
    // for better stability than internal RC oscillator (datasheet §8.3.1).
    std::string writeError;
    if (!writeByte(kPwrMgmt1Register, 0x01, &writeError)) {
        setError(errorMessage, "mpu6050 wake failed: " + writeError);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Step 3: configure DLPF (low-pass filter). DLPF_CFG=3 → 44 Hz BW
    // for both gyro and accel (datasheet §8.3.4).
    const int dlpfCfg = dlpfBandwidthToCfg(config_.dlpfBandwidthHz);
    if (dlpfCfg < 0) {
        setError(errorMessage, "mpu6050 invalid DLPF bandwidth");
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    if (!writeByte(kConfigRegister, static_cast<uint8_t>(dlpfCfg), &writeError)) {
        setError(errorMessage, "mpu6050 DLPF config failed: " + writeError);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Step 4: set gyro full-scale range (GYRO_CONFIG, bits [4:3]).
    if (!writeByte(kGyroConfigRegister,
                   gyroRangeToConfig(config_.gyroRangeDps), &writeError)) {
        setError(errorMessage, "mpu6050 gyro config failed: " + writeError);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Step 5: set accel full-scale range (ACCEL_CONFIG, bits [4:3]).
    if (!writeByte(kAccelConfigRegister,
                   accelRangeToConfig(config_.accelRangeG), &writeError)) {
        setError(errorMessage, "mpu6050 accel config failed: " + writeError);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Step 6: set sample rate divider. With DLPF_CFG=3 (1 kHz internal
    // sample rate), SMPLRT_DIV=19 → 1 kHz / (1 + 19) = 50 Hz.
    // We always target 50 Hz internal rate regardless of host polling.
    if (!writeByte(kSampleRateDivRegister, 19, &writeError)) {
        setError(errorMessage, "mpu6050 sample rate config failed: " + writeError);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Pre-compute scaling factors for engineering-unit conversion.
    const double accelSens = accelSensitivity(config_.accelRangeG);
    const double gyroSens = gyroSensitivity(config_.gyroRangeDps);
    if (accelSens <= 0.0 || gyroSens <= 0.0) {
        setError(errorMessage, "mpu6050 invalid sensitivity");
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    accelScale_ = kGravity / accelSens;     // LSB -> m/s^2
    gyroScale_ = 1.0 / gyroSens;             // LSB -> deg/s

    available_ = true;
    return true;
}

void Mpu6050Device::closeDevice()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    available_ = false;
    accelScale_ = 0.0;
    gyroScale_ = 0.0;
}

Mpu6050Reading Mpu6050Device::readSensorValues(std::string *errorMessage) const
{
    Mpu6050Reading reading;
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    if (!available_) {
        reading.errorMessage = "mpu6050 device not available";
        setError(errorMessage, reading.errorMessage);
        return reading;
    }

    // Bulk read 14 bytes starting at ACCEL_XOUT_H (0x3B):
    //   [0..1]  accel X (H, L)
    //   [2..3]  accel Y
    //   [4..5]  accel Z
    //   [6..7]  temperature (raw = raw/340 + 36.53)
    //   [8..9]  gyro X
    //   [10..11] gyro Y
    //   [12..13] gyro Z
    uint8_t buffer[kRawDataSize];
    std::string readError;
    if (!readBlock(kAccelXoutHRegister, buffer, kRawDataSize, &readError)) {
        reading.errorMessage = readError;
        setError(errorMessage, reading.errorMessage);
        return reading;
    }

    // Big-endian 16-bit signed conversion (MPU6050 datasheet §3.5).
    auto readS16 = [](const uint8_t *p) -> int16_t {
        return static_cast<int16_t>((static_cast<uint16_t>(p[0]) << 8) |
                                     static_cast<uint16_t>(p[1]));
    };

    const int16_t accelRawX = readS16(buffer + 0);
    const int16_t accelRawY = readS16(buffer + 2);
    const int16_t accelRawZ = readS16(buffer + 4);
    const int16_t tempRaw  = readS16(buffer + 6);
    const int16_t gyroRawX = readS16(buffer + 8);
    const int16_t gyroRawY = readS16(buffer + 10);
    const int16_t gyroRawZ = readS16(buffer + 12);

    reading.accelX = static_cast<double>(accelRawX) * accelScale_;
    reading.accelY = static_cast<double>(accelRawY) * accelScale_;
    reading.accelZ = static_cast<double>(accelRawZ) * accelScale_;
    reading.gyroX = static_cast<double>(gyroRawX) * gyroScale_;
    reading.gyroY = static_cast<double>(gyroRawY) * gyroScale_;
    reading.gyroZ = static_cast<double>(gyroRawZ) * gyroScale_;
    reading.temperature = static_cast<double>(tempRaw) / 340.0 + 36.53;
    reading.valid = true;
    return reading;
}

} // namespace vehicledevice
