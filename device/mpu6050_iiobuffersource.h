#ifndef MPU6050_IIOBUFFERSOURCE_H
#define MPU6050_IIOBUFFERSOURCE_H

#include "iimusamplesource.h"
#include "mpu6050device.h"
#include "vehicledeviceconfig.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vehicledevice {

// Mpu6050IioBufferSource — IIO triggered buffer 后端实现。
//
// 通过 /dev/iio:deviceX 字符设备读取连续样本，利用内核 Data Ready IRQ
// 触发采样，避免用户态轮询的抖动和 CPU 占用。
//
// 数据通路：
//   MPU6500 Data Ready IRQ → IIO trigger → 内核 buffer → /dev/iio:deviceX
//   → 本类 read() → 解析样本 → 返回 Mpu6050Reading（m/s^2, deg/s, °C）
//
// 采样率由内核驱动和 DTS 配置决定（MPU6500 默认 50Hz），
// 不是由 QTimer 轮询控制。
//
// 生命周期：
//   Mpu6050IioBufferSource src(config);
//   if (src.open(&err)) {
//       Mpu6050Reading r = src.readSample(&err);
//   }
//   src.close();
//
// 第4周新增，用于 A/B 对比 UserI2c vs IioBuffer 后端。
class Mpu6050IioBufferSource : public IImuSampleSource
{
public:
    explicit Mpu6050IioBufferSource(const Mpu6050Config &config);
    ~Mpu6050IioBufferSource() override;

    Mpu6050IioBufferSource(const Mpu6050IioBufferSource &) = delete;
    Mpu6050IioBufferSource &operator=(const Mpu6050IioBufferSource &) = delete;

    // === IImuSampleSource 实现 ===
    bool open(std::string *errorMessage) override;
    void close() override;
    bool isOpen() const override { return fd_ >= 0; }
    Mpu6050Reading readSample(std::string *errorMessage) override;

private:
    // 查找名字为 "mpu6500" 的 IIO 设备并返回其 sysfs 路径
    // （如 /sys/bus/iio/devices/iio:device2）。失败返回空串。
    std::string findMpu6500Device() const;

    // 读取 sysfs 整数属性（如 in_accel_scale）。
    // 失败返回 false。
    bool readSysfsInt(const std::string &path, int *value) const;

    // 读取 sysfs 浮点属性（如 in_accel_scale）。
    bool readSysfsDouble(const std::string &path, double *value) const;

    // 写 sysfs 字符串属性。
    bool writeSysfs(const std::string &path, const std::string &value) const;

    // 启用所有 scan_elements + 配置 buffer。
    bool enableBuffer(std::string *errorMessage);
    void disableBuffer();

    // 解析 IIO buffer 样本字节布局（scan_elements 的 index/type/endianess）。
    bool parseScanLayout(std::string *errorMessage);

    Mpu6050Config config_;
    int fd_;                    // /dev/iio:deviceX 文件描述符
    std::string sysfsPath_;     // /sys/bus/iio/devices/iio:deviceN

    // IIO 样本布局（parseScanLayout 填充）
    int sampleSize_;            // 单个样本字节数
    int accelChans_[3];         // accel x/y/z 在样本内的偏移（字节）
    int gyroChans_[3];          // anglvel x/y/z 偏移
    int tempChanOffset_;        // temp 偏移，-1 表示无
    int timestampOffset_;       // timestamp 偏移，-1 表示无
    bool accelBigEndian_[3];    // 对应 scan_elements/*_type
    bool gyroBigEndian_[3];
    bool tempBigEndian_;

    // scale（从 sysfs 读取，用于把 raw 转工程单位）
    double accelScale_;         // m/s^2 per LSB
    double gyroScale_;          // deg/s per LSB
    double tempScale_;          // 毫摄氏度 per LSB
    int tempOffset_;            // 温度偏移（IIO ABI: (raw + offset) * scale）

    // 样本读取缓冲区
    std::vector<uint8_t> sampleBuf_;
};

} // namespace vehicledevice

#endif // MPU6050_IIOBUFFERSOURCE_H
