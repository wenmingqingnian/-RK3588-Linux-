#include "vehicledeviceconfig.h"

#include <cctype>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace vehicledevice {
namespace {

std::string trim(const std::string &value)
{
    std::string::size_type first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }

    std::string::size_type last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }

    return value.substr(first, last - first);
}

bool startsWith(const std::string &value, const std::string &prefix)
{
    return value.compare(0, prefix.size(), prefix) == 0;
}

void putKeyValue(std::map<std::string, std::string> *values,
                 const std::string &line,
                 int lineNumber)
{
    const std::string::size_type separator = line.find('=');
    if (separator == std::string::npos) {
        throw DeviceConfigError("invalid device config line " + std::to_string(lineNumber));
    }

    const std::string key = trim(line.substr(0, separator));
    const std::string value = trim(line.substr(separator + 1));
    if (key.empty()) {
        throw DeviceConfigError("empty device config key at line " + std::to_string(lineNumber));
    }
    if (values->find(key) != values->end()) {
        throw DeviceConfigError("duplicate device config key: " + key);
    }

    (*values)[key] = value;
}

bool parseBoolValue(const std::string &value, bool *parsedValue)
{
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        *parsedValue = true;
        return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off") {
        *parsedValue = false;
        return true;
    }
    return false;
}

int parsePositiveInt(const std::string &value, const std::string &key)
{
    char *end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed <= 0 ||
        parsed > static_cast<long>(INT_MAX)) {
        throw DeviceConfigError("invalid positive integer for " + key);
    }
    return static_cast<int>(parsed);
}

int parseRangedInt(const std::string &value,
                   const std::string &key,
                   int minimum,
                   int maximum)
{
    const int parsed = parsePositiveInt(value, key);
    if (parsed < minimum || parsed > maximum) {
        throw DeviceConfigError("invalid range for " + key);
    }
    return parsed;
}

int parseNonNegativeInt(const std::string &value, const std::string &key)
{
    char *end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed < 0 ||
        parsed > static_cast<long>(INT_MAX)) {
        throw DeviceConfigError("invalid non-negative integer for " + key);
    }
    return static_cast<int>(parsed);
}

int parseI2cAddress(const std::string &value, const std::string &key)
{
    int base = 10;
    std::string digits = value;
    if (value.size() > 2 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        base = 16;
        digits = value.substr(2);
    }

    char *end = nullptr;
    const long address = std::strtol(digits.c_str(), &end, base);
    if (end == digits.c_str() || *end != '\0' ||
        address < 0x08 || address > 0x77) {
        throw DeviceConfigError("invalid I2C address for " + key);
    }
    return static_cast<int>(address);
}

char parseGpsParity(const std::string &value)
{
    if (value.size() != 1) {
        throw DeviceConfigError("invalid gps parity");
    }

    const char parity = static_cast<char>(
        std::toupper(static_cast<unsigned char>(value[0])));
    if (parity != 'N' && parity != 'O' && parity != 'E') {
        throw DeviceConfigError("invalid gps parity");
    }
    return parity;
}

// GPS baud rates supported by the termios layer. Values outside this set are
// rejected rather than silently coerced to a default.
bool isAllowedBaudrate(int value)
{
    static const int allowed[] = {1200, 2400, 4800, 9600, 19200, 38400,
                                  57600, 115200, 230400, 460800, 500000};
    for (std::size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        if (allowed[i] == value) {
            return true;
        }
    }
    return false;
}

void rejectUnknownKeys(const std::map<std::string, std::string> &values)
{
    static const char *knownKeys[] = {
        "key_input_enabled",
        "key_input_device",
        "key_input_pressed_value",
        "key_input_poll_interval_ms",
        "gps_enabled",
        "gps_device",
        "gps_baudrate",
        "gps_data_bits",
        "gps_parity",
        "gps_stop_bits",
        "gps_read_timeout_ms",
        "gps_update_interval_ms",
        "gps_simulation_enabled",
        "gps_simulation_file",
        "gps_simulation_interval_ms",
        "gps_map_update_interval_ms",
        "dht11_enabled",
        "dht11_sysfs_dir",
        "dht11_poll_interval_ms",
        "ap3216_enabled",
        "ap3216_backend",
        "ap3216_sysfs_dir",
        "ap3216_als_file",
        "ap3216_ps_file",
        "ap3216_ir_file",
        "ap3216_use_iio_events",
        "ap3216_i2c_device",
        "ap3216_i2c_address",
        "ap3216_poll_interval_ms",
        "ap3216_ps_falling_threshold",
        "ap3216_ps_rising_threshold",
        "buzzer_enabled",
        "buzzer_gpio_chip",
        "buzzer_gpio_line",
        "buzzer_active_low",
        "mpu6050_enabled",
        "mpu6050_i2c_device",
        "mpu6050_i2c_address",
        "mpu6050_poll_interval_ms",
        "mpu6050_gyro_range_dps",
        "mpu6050_accel_range_g",
        "mpu6050_dlpf_bandwidth_hz",
        "mpu6050_backend"
    };

    for (std::map<std::string, std::string>::const_iterator it = values.begin();
         it != values.end();
         ++it) {
        bool known = false;
        for (std::size_t index = 0; index < sizeof(knownKeys) / sizeof(knownKeys[0]); ++index) {
            if (it->first == knownKeys[index]) {
                known = true;
                break;
            }
        }
        if (!known) {
            throw DeviceConfigError("unknown device config key: " + it->first);
        }
    }
}

} // namespace

DeviceConfigError::DeviceConfigError(const std::string &message)
    : std::runtime_error(message)
{
}

KeyInputConfig::KeyInputConfig()
    : enabled(false),
      devicePath(),
      pressedValue(240),
      pollIntervalMs(500)
{
}

GpsSerialConfig::GpsSerialConfig()
    : enabled(false),
      devicePath(),
      baudrate(9600),
      dataBits(8),
      parity('N'),
      stopBits(1),
      readTimeoutMs(500),
      updateIntervalMs(1000)
{
}

// Defaults: simulation disabled, no file, 500 ms playback interval, 1000 ms
// map refresh. Both real GPS and simulation default off so the shipping
// configuration never accesses the serial port or a simulation file.
GpsSimulationConfig::GpsSimulationConfig()
    : enabled(false),
      file(),
      intervalMs(500),
      mapUpdateIntervalMs(1000)
{
}

Dht11Config::Dht11Config()
    : enabled(false),
      sysfsDir(),
      pollIntervalMs(2000)
{
}

Ap3216Config::Ap3216Config()
    : enabled(false),
      backend("iio"),
      sysfsDir(),
      alsFile("in_illuminance_raw"),
      psFile("in_proximity_raw"),
      irFile("in_intensity_ir_raw"),
      useIioEvents(true),
      i2cDevice("/dev/i2c-3"),
      i2cAddress(0x1e),
      pollIntervalMs(500),
      psFallingThreshold(50),
      psRisingThreshold(100)
{
}

// Defaults: buzzer disabled, active-low trigger (low=beep).
BuzzerConfig::BuzzerConfig()
    : enabled(false),
      gpioChip(),
      gpioLine(0),
      activeLow(true)
{
}

// Defaults: MPU6050 on I2C3 (QuarkPi-CA2 J11 40Pin Pin 3=SDA, Pin 5=SCL),
// address 0x68, 50 Hz polling, ±500 dps gyro, ±4g accel, 44 Hz DLPF.
// Disabled by default until the user wires the hardware and verifies
// `i2cdetect -y 3` shows 0x68.
Mpu6050Config::Mpu6050Config()
    : enabled(false),
      i2cDevice("/dev/i2c-3"),
      i2cAddress(0x68),
      pollIntervalMs(20),
      gyroRangeDps(500),
      accelRangeG(4),
      dlpfBandwidthHz(44),
      backend("user_i2c")
{
}

VehicleDeviceConfig parseDeviceConfigText(const std::string &configText)
{
    std::map<std::string, std::string> values;
    std::istringstream stream(configText);
    std::string line;
    int lineNumber = 0;

    while (std::getline(stream, line)) {
        ++lineNumber;
        const std::string trimmedLine = trim(line);
        if (trimmedLine.empty() || startsWith(trimmedLine, "#")) {
            continue;
        }
        putKeyValue(&values, trimmedLine, lineNumber);
    }

    rejectUnknownKeys(values);

    VehicleDeviceConfig config;
    std::map<std::string, std::string>::const_iterator found =
        values.find("key_input_enabled");
    if (found != values.end() &&
        !parseBoolValue(found->second, &config.keyInput.enabled)) {
        throw DeviceConfigError("invalid boolean for key_input_enabled");
    }

    found = values.find("key_input_device");
    if (found != values.end()) {
        config.keyInput.devicePath = found->second;
    }

    found = values.find("key_input_pressed_value");
    if (found != values.end()) {
        config.keyInput.pressedValue =
            parsePositiveInt(found->second, "key_input_pressed_value");
    }

    found = values.find("key_input_poll_interval_ms");
    if (found != values.end()) {
        config.keyInput.pollIntervalMs =
            parsePositiveInt(found->second, "key_input_poll_interval_ms");
    }

    if (config.keyInput.enabled && config.keyInput.devicePath.empty()) {
        throw DeviceConfigError("key_input_device is required when key input is enabled");
    }

    found = values.find("gps_enabled");
    if (found != values.end() &&
        !parseBoolValue(found->second, &config.gps.enabled)) {
        throw DeviceConfigError("invalid boolean for gps_enabled");
    }

    found = values.find("gps_device");
    if (found != values.end()) {
        config.gps.devicePath = found->second;
    }

    found = values.find("gps_baudrate");
    if (found != values.end()) {
        config.gps.baudrate = parsePositiveInt(found->second, "gps_baudrate");
        if (!isAllowedBaudrate(config.gps.baudrate)) {
            throw DeviceConfigError("invalid gps_baudrate (not in allowed list)");
        }
    }

    found = values.find("gps_data_bits");
    if (found != values.end()) {
        config.gps.dataBits =
            parseRangedInt(found->second, "gps_data_bits", 5, 8);
    }

    found = values.find("gps_parity");
    if (found != values.end()) {
        config.gps.parity = parseGpsParity(found->second);
    }

    found = values.find("gps_stop_bits");
    if (found != values.end()) {
        config.gps.stopBits =
            parseRangedInt(found->second, "gps_stop_bits", 1, 2);
    }

    found = values.find("gps_read_timeout_ms");
    if (found != values.end()) {
        // 1 ms .. 60 s: bounded to reject zero/negative and absurd values.
        config.gps.readTimeoutMs =
            parseRangedInt(found->second, "gps_read_timeout_ms", 1, 60000);
    }

    found = values.find("gps_update_interval_ms");
    if (found != values.end()) {
        config.gps.updateIntervalMs =
            parseRangedInt(found->second, "gps_update_interval_ms", 1, 60000);
    }

    if (config.gps.enabled && config.gps.devicePath.empty()) {
        throw DeviceConfigError("gps_device is required when gps is enabled");
    }

    // --- GPS simulation ---
    found = values.find("gps_simulation_enabled");
    if (found != values.end() &&
        !parseBoolValue(found->second, &config.gpsSimulation.enabled)) {
        throw DeviceConfigError("invalid boolean for gps_simulation_enabled");
    }

    found = values.find("gps_simulation_file");
    if (found != values.end()) {
        config.gpsSimulation.file = found->second;
    }

    found = values.find("gps_simulation_interval_ms");
    if (found != values.end()) {
        config.gpsSimulation.intervalMs =
            parseRangedInt(found->second, "gps_simulation_interval_ms", 1, 60000);
    }

    found = values.find("gps_map_update_interval_ms");
    if (found != values.end()) {
        config.gpsSimulation.mapUpdateIntervalMs =
            parseRangedInt(found->second, "gps_map_update_interval_ms", 1, 60000);
    }

    // Real GPS and simulation are mutually exclusive. Enabling both is a
    // configuration error and must fail loudly, never silently pick one.
    if (config.gps.enabled && config.gpsSimulation.enabled) {
        throw DeviceConfigError(
            "gps_enabled and gps_simulation_enabled cannot both be true");
    }

    if (config.gpsSimulation.enabled && config.gpsSimulation.file.empty()) {
        throw DeviceConfigError(
            "gps_simulation_file is required when gps_simulation is enabled");
    }

    found = values.find("dht11_enabled");
    if (found != values.end() &&
        !parseBoolValue(found->second, &config.dht11.enabled)) {
        throw DeviceConfigError("invalid boolean for dht11_enabled");
    }

    found = values.find("dht11_sysfs_dir");
    if (found != values.end()) {
        config.dht11.sysfsDir = found->second;
    }

    found = values.find("dht11_poll_interval_ms");
    if (found != values.end()) {
        config.dht11.pollIntervalMs =
            parsePositiveInt(found->second, "dht11_poll_interval_ms");
    }

    // dht11_sysfs_dir 可以为空——空时 Dht11Device::openDevice() 会自动
    // 扫描 /sys/bus/iio/devices/iio:device*/name=="dht11" 发现设备。
    // 这里不强制非空，避免阻止自动扫描路径从正常配置入口启用。

    found = values.find("ap3216_enabled");
    if (found != values.end() &&
        !parseBoolValue(found->second, &config.ap3216.enabled)) {
        throw DeviceConfigError("invalid boolean for ap3216_enabled");
    }

    found = values.find("ap3216_backend");
    if (found != values.end()) {
        if (found->second != "iio" && found->second != "user_i2c") {
            throw DeviceConfigError(
                "ap3216_backend must be iio or user_i2c");
        }
        config.ap3216.backend = found->second;
    }

    found = values.find("ap3216_sysfs_dir");
    if (found != values.end()) {
        config.ap3216.sysfsDir = found->second;
    }

    found = values.find("ap3216_als_file");
    if (found != values.end()) {
        config.ap3216.alsFile = found->second;
    }

    found = values.find("ap3216_ps_file");
    if (found != values.end()) {
        config.ap3216.psFile = found->second;
    }

    found = values.find("ap3216_ir_file");
    if (found != values.end()) {
        config.ap3216.irFile = found->second;
    }

    found = values.find("ap3216_use_iio_events");
    if (found != values.end() &&
        !parseBoolValue(found->second, &config.ap3216.useIioEvents)) {
        throw DeviceConfigError("invalid boolean for ap3216_use_iio_events");
    }

    found = values.find("ap3216_i2c_device");
    if (found != values.end()) {
        config.ap3216.i2cDevice = found->second;
    }

    found = values.find("ap3216_i2c_address");
    if (found != values.end()) {
        config.ap3216.i2cAddress =
            parseI2cAddress(found->second, "ap3216_i2c_address");
    }

    found = values.find("ap3216_poll_interval_ms");
    if (found != values.end()) {
        config.ap3216.pollIntervalMs =
            parseRangedInt(found->second,
                           "ap3216_poll_interval_ms", 50, 60000);
    }

    found = values.find("ap3216_ps_falling_threshold");
    if (found != values.end()) {
        config.ap3216.psFallingThreshold =
            parseNonNegativeInt(found->second,
                                "ap3216_ps_falling_threshold");
    }

    found = values.find("ap3216_ps_rising_threshold");
    if (found != values.end()) {
        config.ap3216.psRisingThreshold =
            parseNonNegativeInt(found->second,
                                "ap3216_ps_rising_threshold");
    }

    if (config.ap3216.psFallingThreshold > 1023 ||
        config.ap3216.psRisingThreshold > 1023 ||
        config.ap3216.psFallingThreshold >=
            config.ap3216.psRisingThreshold) {
        throw DeviceConfigError(
            "ap3216 PS thresholds require 0 <= falling < rising <= 1023");
    }

    if (config.ap3216.backend == "iio" &&
        (config.ap3216.alsFile.empty() ||
         config.ap3216.psFile.empty() ||
         config.ap3216.irFile.empty())) {
        throw DeviceConfigError(
            "ap3216 IIO channel file names must not be empty");
    }

    if (config.ap3216.enabled &&
        config.ap3216.backend == "user_i2c" &&
        config.ap3216.i2cDevice.empty()) {
        throw DeviceConfigError(
            "ap3216_i2c_device is required for user_i2c backend");
    }

    found = values.find("buzzer_enabled");
    if (found != values.end() &&
        !parseBoolValue(found->second, &config.buzzer.enabled)) {
        throw DeviceConfigError("invalid boolean for buzzer_enabled");
    }

    found = values.find("buzzer_gpio_chip");
    if (found != values.end()) {
        config.buzzer.gpioChip = found->second;
    }

    found = values.find("buzzer_gpio_line");
    if (found != values.end()) {
        config.buzzer.gpioLine =
            parseNonNegativeInt(found->second, "buzzer_gpio_line");
    }

    found = values.find("buzzer_active_low");
    if (found != values.end() &&
        !parseBoolValue(found->second, &config.buzzer.activeLow)) {
        throw DeviceConfigError("invalid boolean for buzzer_active_low");
    }

    if (config.buzzer.enabled && config.buzzer.gpioChip.empty()) {
        throw DeviceConfigError("buzzer_gpio_chip is required when buzzer is enabled");
    }

    // --- MPU6050 ---
    found = values.find("mpu6050_enabled");
    if (found != values.end() &&
        !parseBoolValue(found->second, &config.mpu6050.enabled)) {
        throw DeviceConfigError("invalid boolean for mpu6050_enabled");
    }

    found = values.find("mpu6050_i2c_device");
    if (found != values.end()) {
        config.mpu6050.i2cDevice = found->second;
    }

    found = values.find("mpu6050_i2c_address");
    if (found != values.end()) {
        // Accept decimal (104) or hex with 0x prefix (0x68).
        const std::string &addrStr = found->second;
        int base = 10;
        std::string numPart = addrStr;
        if (addrStr.size() > 2 && addrStr[0] == '0' &&
            (addrStr[1] == 'x' || addrStr[1] == 'X')) {
            base = 16;
            numPart = addrStr.substr(2);
        }
        char *end = nullptr;
        const long addr = std::strtol(numPart.c_str(), &end, base);
        if (end == numPart.c_str() || *end != '\0' ||
            addr < 0x08 || addr > 0x77) {
            throw DeviceConfigError("invalid I2C address for mpu6050_i2c_address");
        }
        config.mpu6050.i2cAddress = static_cast<int>(addr);
    }

    found = values.find("mpu6050_poll_interval_ms");
    if (found != values.end()) {
        config.mpu6050.pollIntervalMs =
            parsePositiveInt(found->second, "mpu6050_poll_interval_ms");
    }

    found = values.find("mpu6050_gyro_range_dps");
    if (found != values.end()) {
        const int range = parsePositiveInt(found->second, "mpu6050_gyro_range_dps");
        if (range != 250 && range != 500 && range != 1000 && range != 2000) {
            throw DeviceConfigError("mpu6050_gyro_range_dps must be 250/500/1000/2000");
        }
        config.mpu6050.gyroRangeDps = range;
    }

    found = values.find("mpu6050_accel_range_g");
    if (found != values.end()) {
        const int range = parsePositiveInt(found->second, "mpu6050_accel_range_g");
        if (range != 2 && range != 4 && range != 8 && range != 16) {
            throw DeviceConfigError("mpu6050_accel_range_g must be 2/4/8/16");
        }
        config.mpu6050.accelRangeG = range;
    }

    found = values.find("mpu6050_dlpf_bandwidth_hz");
    if (found != values.end()) {
        const int bw = parsePositiveInt(found->second, "mpu6050_dlpf_bandwidth_hz");
        // DLPF_CFG mapping: 0=260, 1=188, 2=94, 3=44, 4=21, 5=10, 6=5, 7=reserved
        if (bw != 260 && bw != 188 && bw != 94 && bw != 44 &&
            bw != 21 && bw != 10 && bw != 5) {
            throw DeviceConfigError("mpu6050_dlpf_bandwidth_hz must be 5/10/21/44/94/188/260");
        }
        config.mpu6050.dlpfBandwidthHz = bw;
    }

    found = values.find("mpu6050_backend");
    if (found != values.end()) {
        const std::string &backend = found->second;
        if (backend != "user_i2c" && backend != "iio_buffer") {
            throw DeviceConfigError("mpu6050_backend must be user_i2c or iio_buffer");
        }
        config.mpu6050.backend = backend;
    }

    if (config.mpu6050.enabled && config.mpu6050.backend == "user_i2c"
        && config.mpu6050.i2cDevice.empty()) {
        throw DeviceConfigError("mpu6050_i2c_device is required when mpu6050 is enabled");
    }

    return config;
}

VehicleDeviceConfig loadDeviceConfigFile(const std::string &path,
                                         bool *loaded,
                                         std::string *errorMessage)
{
    if (loaded != nullptr) {
        *loaded = false;
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        if (errorMessage != nullptr) {
            *errorMessage = "device config file not found: " + path;
        }
        return VehicleDeviceConfig();
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();

    try {
        VehicleDeviceConfig config = parseDeviceConfigText(buffer.str());
        if (loaded != nullptr) {
            *loaded = true;
        }
        return config;
    } catch (const DeviceConfigError &e) {
        if (errorMessage != nullptr) {
            *errorMessage = "device config parse error: " + std::string(e.what());
        }
        return VehicleDeviceConfig();
    }
}

} // namespace vehicledevice
