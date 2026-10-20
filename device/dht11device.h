#ifndef DHT11DEVICE_H
#define DHT11DEVICE_H

#include "vehicledeviceconfig.h"

#include <string>

namespace vehicledevice {

// DHT11 一次读取结果。valid=true 时 temperature/humidity 才有意义。
// temperature 单位：摄氏度（°C），humidity 单位：百分相对湿度（%RH）。
struct Dht11Reading {
    bool valid;
    double temperature;
    double humidity;
    std::string errorMessage;

    Dht11Reading();
};

// Dht11Device —— 通过 Linux IIO sysfs 读取 DHT11 或 SHT3x 温湿度数据。
//
// DHT11 IIO ABI：
//   in_temp_input / in_humidityrelative_input，单位均为毫工程单位。
// SHT3x 本地 IIO 驱动 ABI：
//   in_temp_raw + in_temp_scale，温度(°C) = (raw * scale - 45000) / 1000；
//   in_humidityrelative_raw + scale，湿度(%RH) = raw * scale。
//
// 通道文件名固定为类内常量，不暴露为配置项。
//
// openDevice() 行为：
//   - enabled=false：直接返回 true，不触碰 sysfs。
//   - sysfsDir 非空：读 {sysfsDir}/name 验证 == "dht11"，不触发实际采样。
//   - sysfsDir 为空：扫描 /sys/bus/iio/devices/iio:device*/name，
//                    找到第一个 name=="dht11" 的设备并记录其路径。
//                    找到 0 个返回 false；找到多个选第一个并写 warning。
// openDevice() 成功后 isAvailable() 返回 true，但**不保证**后续
// readTemperatureHumidity() 一定成功——采样失败由 readTemperatureHumidity()
// 自行报告，调用方应据此更新可用性状态。
//
// 线程安全：非线程安全。调用方（Dht11Worker）保证单线程访问。
class Dht11Device {
public:
    explicit Dht11Device(const Dht11Config &config);
    ~Dht11Device();

    // 校验 sysfs 节点身份（读 name 文件）。enabled=false 时直接返回 true。
    // 不会触发实际传感器采样。成功后 isAvailable() 返回 true。
    bool openDevice(std::string *errorMessage);
    void closeDevice();
    bool isAvailable() const;

    // 读取温湿度。失败时 reading.valid=false 且 reading.errorMessage 有值。
    // 失败原因包括：设备未 open、sysfs 文件打开失败、read 失败、
    // 文本非整数、IIO 驱动返回负 errno（通过 read() 失败 errno 体现）。
    Dht11Reading readTemperatureHumidity(std::string *errorMessage) const;

    // DHT11 processed 通道文件名（保留旧测试/API）。
    static const char *const kTempFile;
    static const char *const kHumidityFile;
    // SHT3x raw + scale 通道文件名。
    static const char *const kSht3xTempRawFile;
    static const char *const kSht3xTempScaleFile;
    static const char *const kSht3xHumidityRawFile;
    static const char *const kSht3xHumidityScaleFile;
    static const char *const kNameFile;
    static const char *const kExpectedName;       // "dht11"，保留旧测试/API
    static const char *const kSht3xExpectedName;  // "sht3x"
    static const char *const kIioDevicesDir;      // /sys/bus/iio/devices

private:
    Dht11Config config_;
    bool available_;
    // 运行时解析得到的 sysfs 路径。
    // - config_.sysfsDir 非空时，等于 config_.sysfsDir；
    // - config_.sysfsDir 为空时，每次 openDevice() 重新扫描发现，
    //   避免设备重新编号后写入旧路径导致后续永远打不开。
    // closeDevice() 清空此字段，强制下次重新解析。
    std::string resolvedSysfsDir_;
    std::string resolvedDeviceName_;

    Dht11Device(const Dht11Device &);
    Dht11Device &operator=(const Dht11Device &);
};

} // namespace vehicledevice

#endif // DHT11DEVICE_H
