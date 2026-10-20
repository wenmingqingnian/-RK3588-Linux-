#ifndef IIMUSAMPLESOURCE_H
#define IIMUSAMPLESOURCE_H


#include <string>

namespace vehicledevice {

// MPU6050 6-axis IMU raw sample (accelerometer + gyroscope + temperature).
// All values are in engineering units (m/s^2, deg/s, °C), already converted
// from raw 16-bit signed integers using the configured range.
//
// Axis orientation (chip flat, Z-up):
//   X: roll axis (left/right tilt)
//   Y: pitch axis (forward/backward tilt)
//   Z: yaw axis (gravity vector when level)
//
// Temperature is derived from the on-die thermistor; not lab-grade but
// sufficient for drift compensation in vehicle cabin (-40~+85°C range).
struct Mpu6050Reading {
    bool valid;
    double accelX;     // m/s^2
    double accelY;     // m/s^2
    double accelZ;     // m/s^2
    double gyroX;      // deg/s
    double gyroY;      // deg/s
    double gyroZ;      // deg/s
    double temperature;  // °C
    std::string errorMessage;

    Mpu6050Reading();
};


// IImuSampleSource — IMU 数据源抽象接口。
//
// 第4周引入，用于解耦 Mpu6050Worker 与具体数据来源：
//   - Mpu6050UserI2cSource: 旧实现（读 /dev/i2c-3 + i2c_smbus_*）
//   - Mpu6050IioBufferSource: 新实现（读 /dev/iio:deviceX triggered buffer）
//
// 两种后端返回相同的 Mpu6050Reading（m/s^2, deg/s, °C），
// Worker 层不感知数据来源，便于 A/B 对比和后续切换。
//
// 生命周期：
//   IImuSampleSource *src = createBackend(config);
//   if (src->open(&err)) {
//       Mpu6050Reading r = src->readSample(&err);
//       if (r.valid) { /* use r */ }
//   }
//   src->close();
class IImuSampleSource
{
public:
    virtual ~IImuSampleSource() = default;

    // 打开数据源（I2C 设备或 IIO 字符设备 + 配置寄存器）。
    // 失败时返回 false 并设置 errorMessage。
    // 幂等：对已打开的源调用会先 close 再重新 open。
    virtual bool open(std::string *errorMessage) = 0;

    // 关闭数据源。幂等。
    virtual void close() = 0;

    // 数据源是否就绪可读。
    virtual bool isOpen() const = 0;

    // 读取一个样本。
    // 成功: reading.valid = true, 值为工程单位。
    // 失败: reading.valid = false, reading.errorMessage 设置。
    // 调用方应串行化调用（非线程安全）。
    virtual Mpu6050Reading readSample(std::string *errorMessage) = 0;
};

} // namespace vehicledevice

#endif // IIMUSAMPLESOURCE_H
