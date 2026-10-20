#ifndef MPU6050DEVICE_H
#define MPU6050DEVICE_H

#include "iimusamplesource.h"
#include "vehicledeviceconfig.h"

#include <cstdint>
#include <string>

namespace vehicledevice {

// Mpu6050Device — raw user-space I2C driver for MPU6050 / MPU9250 IMU.
//
// Supports both MPU6050 (WHO_AM_I=0x68) and MPU9250 (WHO_AM_I=0x70/0x71).
// MPU9250's 6-axis part (accel + gyro) is register-compatible with MPU6050;
// the AK8963 magnetometer on MPU9250's auxiliary I2C bus is NOT used by
// this driver (we only need pitch/roll, no yaw).
//
// Uses /dev/i2c-N + i2c_smbus_* API (no kernel driver required).
// On open: wakes the device, sets sample rate, DLPF, and full-scale ranges.
// On read: bulk-reads 14 bytes starting at ACCEL_XOUT_H (0x3B).
//
// Thread safety: not thread-safe. Caller (Mpu6050Worker) must serialize
// openDevice / readSensorValues / closeDevice calls.
//
// Lifecycle:
//   Mpu6050Device dev(config);
//   std::string err;
//   if (dev.openDevice(&err)) {
//       Mpu6050Reading r = dev.readSensorValues(&err);
//       if (r.valid) { /* use r.accelX, r.gyroZ, ... */ }
//   }
//   dev.closeDevice();  // idempotent; safe to call after failed open
//
// 第4周：实现 IImuSampleSource 接口，作为 UserI2c 后端。
// 保留原 openDevice/readSensorValues/closeDevice 方法名，
// 通过 inline 转发到 IImuSampleSource 的 open/readSample/close。
class Mpu6050Device : public IImuSampleSource
{
public:
    explicit Mpu6050Device(const Mpu6050Config &config);
    ~Mpu6050Device() override;

    Mpu6050Device(const Mpu6050Device &) = delete;
    Mpu6050Device &operator=(const Mpu6050Device &) = delete;

    // === IImuSampleSource 实现 ===
    bool open(std::string *errorMessage) override { return openDevice(errorMessage); }
    void close() override { closeDevice(); }
    bool isOpen() const override { return isAvailable(); }
    Mpu6050Reading readSample(std::string *errorMessage) override
    { return readSensorValues(errorMessage); }

    // === 旧 API 保留（向后兼容） ===
    // Opens /dev/i2c-N, sets I2C_SLAVE address, then wakes the device
    // and configures sample rate / DLPF / ranges. Returns false on I2C
    // communication failure (errorMessage set to a descriptive string).
    // Idempotent: calling openDevice on an already-open device first
    // closes the previous fd (allows re-open after transient failure).
    bool openDevice(std::string *errorMessage);

    // Closes the I2C fd. Idempotent (safe to call after failed open).
    void closeDevice();

    bool isAvailable() const { return available_; }

    // Reads 14 bytes (accel 6 + temp 2 + gyro 6) from the sensor.
    // On success: reading.valid = true, values in engineering units.
    // On failure: reading.valid = false, reading.errorMessage set.
    Mpu6050Reading readSensorValues(std::string *errorMessage) const;

    // Sanity constants exposed for tests / diagnostics.
    static constexpr int kWhoAmIRegister = 0x75;
    static constexpr int kWhoAmIExpectedValue = 0x68;       // MPU6050
    static constexpr int kWhoAmIAcceptedAltValue = 0x70;    // MPU9250 (clone variant)
    // Note: genuine MPU9250 returns 0x71, but many clone modules (incl.
    // common GY-9255 boards) return 0x70. Both are accepted here.
    static constexpr int kAccelXoutHRegister = 0x3B;
    static constexpr int kPwrMgmt1Register = 0x6B;
    static constexpr int kConfigRegister = 0x1A;
    static constexpr int kGyroConfigRegister = 0x1B;
    static constexpr int kAccelConfigRegister = 0x1C;
    static constexpr int kSampleRateDivRegister = 0x19;
    static constexpr int kRawDataSize = 14;  // 7 regs * 2 bytes

private:
    // Reads a single byte register via SMBus.
    // Returns -1 on I2C error; caller fills errorMessage.
    int readByte(uint8_t reg, std::string *errorMessage) const;

    // Writes a single byte register via SMBus.
    // Returns false on I2C error; caller fills errorMessage.
    bool writeByte(uint8_t reg, uint8_t value, std::string *errorMessage) const;

    // Bulk reads `length` bytes starting at `reg` via i2c_smbus_read_i2c_block_data.
    // Returns false on I2C error or short read.
    bool readBlock(uint8_t reg, uint8_t *buffer, uint8_t length,
                   std::string *errorMessage) const;

    // Converts DLPF bandwidth (Hz) to DLPF_CFG register value.
    // Returns -1 for invalid bandwidth.
    static int dlpfBandwidthToCfg(int bandwidthHz);

    // Converts gyro range (±dps) to GYRO_CONFIG register bits.
    static uint8_t gyroRangeToConfig(int rangeDps);

    // Converts accel range (±g) to ACCEL_CONFIG register bits.
    static uint8_t accelRangeToConfig(int rangeG);

    // Scaling factors (computed at open time to avoid per-sample division).
    double accelScale_;  // m/s^2 per LSB
    double gyroScale_;   // deg/s per LSB

    Mpu6050Config config_;
    int fd_;
    bool available_;
};

} // namespace vehicledevice

#endif // MPU6050DEVICE_H
