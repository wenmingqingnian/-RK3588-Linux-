#ifndef VEHICLEDEVICECONFIG_H
#define VEHICLEDEVICECONFIG_H

#include <stdexcept>
#include <string>

namespace vehicledevice {

class DeviceConfigError : public std::runtime_error {
public:
    explicit DeviceConfigError(const std::string &message);
};

struct KeyInputConfig {
    bool enabled;
    std::string devicePath;
    int pressedValue;
    int pollIntervalMs;

    KeyInputConfig();
};

struct GpsSerialConfig {
    bool enabled;
    std::string devicePath;
    int baudrate;
    int dataBits;
    char parity;
    int stopBits;
    int readTimeoutMs;
    int updateIntervalMs;

    GpsSerialConfig();
};

// GPS simulation playback configuration. Simulation reads an NMEA text file
// through the same parser as the real serial device and yields fixes marked
// simulated=true. Real GPS and simulation are mutually exclusive: enabling
// both at once is rejected at parse time (fail-closed, never silent).
struct GpsSimulationConfig {
    bool enabled;
    std::string file;
    int intervalMs;
    int mapUpdateIntervalMs;

    GpsSimulationConfig();
};

struct Dht11Config {
    bool enabled;
    // Linux 主线 DHT11 驱动 (drivers/iio/humidity/dht11.c) 是 IIO 子系统，
    // 不创建 /dev/dht11 字符设备，数据通过 sysfs 暴露：
    //   {sysfsDir}/in_temp_input            (毫摄氏度，如 25000 = 25.0°C)
    //   {sysfsDir}/in_humidityrelative_input (毫百分，如 60000 = 60.0%RH)
    // sysfsDir 形如 /sys/bus/iio/devices/iio:device0
    // 通道文件名由 Dht11Device 内部常量固定，不暴露为配置项，
    // 避免用户填错或被未知键白名单拒绝。
    // sysfsDir 为空时 Dht11Device 会自动扫描
    // /sys/bus/iio/devices/iio:device*/name == "dht11" 的设备。
    std::string sysfsDir;
    int pollIntervalMs;

    Dht11Config();
};

struct Ap3216Config {
    bool enabled;

    // Startup-selected backend: "iio" or "user_i2c". Runtime hot-switching is
    // intentionally unsupported because both paths target the same 0x1e device.
    std::string backend;

    // IIO backend. Empty sysfsDir enables discovery by name=="ap3216c".
    std::string sysfsDir;
    std::string alsFile;
    std::string psFile;
    std::string irFile;
    bool useIioEvents;

    // User-I2C backend. The kernel ap3216c driver must be unbound first.
    std::string i2cDevice;
    int i2cAddress;

    // Shared polling interval and proximity hysteresis thresholds.
    int pollIntervalMs;
    int psFallingThreshold;
    int psRisingThreshold;

    Ap3216Config();
};

struct BuzzerConfig {
    bool enabled;
    std::string gpioChip;
    int gpioLine;
    bool activeLow;

    BuzzerConfig();
};

// MPU6050 6-axis IMU (3-axis accelerometer + 3-axis gyroscope + temp).
// Uses raw user-space I2C via /dev/i2c-N + i2c_smbus_* API (no kernel
// driver required). Default address 0x68 (AD0 pin low).
//
// Sample rate: 50 Hz host polling is sufficient for vehicle attitude
// estimation (driver sets 1 kHz internal sample rate + DLPF_CFG=3).
//
// Ranges: ±4g accelerometer + ±500 dps gyroscope are good trade-offs
// for vehicle scenarios (cover ±5g braking/turning, ±400°/s yaw rates).
struct Mpu6050Config {
    bool enabled;
    std::string i2cDevice;        // /dev/i2c-8 (I2C8_M1 on J11 40Pin)
    int i2cAddress;                // 0x68 (AD0=GND) or 0x69 (AD0=VCC)
    int pollIntervalMs;            // 20ms = 50 Hz
    int gyroRangeDps;              // ±250/±500/±1000/±2000 dps
    int accelRangeG;               // ±2/±4/±8/±16 g
    int dlpfBandwidthHz;           // DLPF_CFG: 0=260Hz / 3=44Hz / 6=5Hz
    // 第4周：数据源后端选择。
    // "user_i2c"  → Mpu6050Device（读 /dev/i2c-N + i2c_smbus_*，旧实现）
    // "iio_buffer" → Mpu6050IioBufferSource（读 /dev/iio:deviceX，triggered buffer）
    // 空串或未识别 → user_i2c（默认）
    std::string backend;

    Mpu6050Config();
};

struct VehicleDeviceConfig {
    KeyInputConfig keyInput;
    GpsSerialConfig gps;
    GpsSimulationConfig gpsSimulation;
    Dht11Config dht11;
    Ap3216Config ap3216;
    BuzzerConfig buzzer;
    Mpu6050Config mpu6050;
};

VehicleDeviceConfig parseDeviceConfigText(const std::string &configText);
VehicleDeviceConfig loadDeviceConfigFile(const std::string &path,
                                         bool *loaded,
                                         std::string *errorMessage);

} // namespace vehicledevice

#endif // VEHICLEDEVICECONFIG_H
