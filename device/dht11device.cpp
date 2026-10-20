#include "dht11device.h"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <string>

// Windows 主机构建兼容：MinGW730 的 fcntl.h 不定义 O_CLOEXEC
// （Linux 2.6.23+ 才有，POSIX.1-2008 标准化）。Windows 没有 fork/exec，
// O_CLOEXEC 无意义，定义为 0 保证源码能编译通过；功能在 Linux 上不受影响。
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace vehicledevice {
namespace {

// 连续读取 sysfs 文本文件的缓冲区大小。IIO 通道值最长约 11 字节
// （"-2147483648" + 换行），64 字节足够。
const std::size_t kReadBufferSize = 64;

void setError(std::string *errorMessage, const std::string &message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

std::string trimLineEnding(const std::string &value)
{
    std::string::size_type last = value.size();
    while (last > 0 && (value[last - 1] == '\n' || value[last - 1] == '\r')) {
        --last;
    }
    return value.substr(0, last);
}

std::string joinPath(const std::string &directory, const std::string &name)
{
    if (directory.empty()) {
        return name;
    }
    const char last = directory[directory.size() - 1];
    if (last == '/' || last == '\\') {
        return directory + name;
    }
    return directory + "/" + name;
}

// 使用 POSIX open/read/close 读取 sysfs 文本文件。
// 失败时通过 errno 输出真实错误原因（IIO 驱动返回负 errno 时，
// sysfs 的 read() 会失败并设置 errno，不会生成负数字符串）。
// 成功返回 true 且 *value 为 trim 后的字符串。
bool readSysfsFile(const std::string &path,
                   std::string *value,
                   std::string *errorMessage)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        setError(errorMessage,
                 "dht11 open failed: " + path + ": " +
                     std::strerror(errno));
        return false;
    }

    char buffer[kReadBufferSize];
    std::string accumulated;
    while (true) {
        const ssize_t count = ::read(fd, buffer, sizeof(buffer) - 1);
        if (count < 0) {
            const int readErrno = errno;
            ::close(fd);
            setError(errorMessage,
                     "dht11 read failed: " + path + ": " +
                         std::strerror(readErrno));
            return false;
        }
        if (count == 0) {
            break;
        }
        accumulated.append(buffer, static_cast<std::size_t>(count));
        // sysfs 文件通常一次 read 返回全部内容；循环避免罕见分块。
        if (accumulated.size() > 256) {
            // 防御性上限：IIO 通道值不可能这么长，避免异常文件无限读。
            ::close(fd);
            setError(errorMessage,
                     "dht11 sysfs value too long: " + path);
            return false;
        }
    }

    ::close(fd);
    *value = trimLineEnding(accumulated);
    return true;
}

// 把 sysfs 读到的文本（如 "25000"）解析为毫值整数。
// 成功返回 true 且 *outMilliValue >= 0；失败返回 false。
// 注意：IIO 驱动采样失败时，read() 会失败（errno 已由 readSysfsFile 报告），
// 不会返回负数字符串。这里保留 value<0 的防御检查仅用于异常文本防护。
bool parseMilliValue(const std::string &text,
                     long *outMilliValue,
                     std::string *errorMessage)
{
    if (text.empty()) {
        setError(errorMessage, "dht11 sysfs value is empty");
        return false;
    }

    errno = 0;
    char *endPtr = nullptr;
    const long value = std::strtol(text.c_str(), &endPtr, 10);
    if (errno != 0 || endPtr == text.c_str() || *endPtr != '\0') {
        setError(errorMessage, "dht11 sysfs value is not an integer: " + text);
        return false;
    }

    if (value < 0) {
        // 防御性检查：理论上 IIO 采样失败通过 read() errno 报告，
        // 不会返回负数字符串。若出现负数文本，按采样失败处理。
        setError(errorMessage,
                 "dht11 sampling failed (unexpected negative value: " +
                     std::to_string(value) + ")");
        return false;
    }

    *outMilliValue = value;
    return true;
}

bool parseDoubleValue(const std::string &text,
                      double *outValue,
                      std::string *errorMessage)
{
    if (text.empty()) {
        setError(errorMessage, "IIO sysfs value is empty");
        return false;
    }

    errno = 0;
    char *endPtr = nullptr;
    const double value = std::strtod(text.c_str(), &endPtr);
    if (errno != 0 || endPtr == text.c_str() || *endPtr != '\0') {
        setError(errorMessage, "IIO sysfs value is not numeric: " + text);
        return false;
    }

    *outValue = value;
    return true;
}

bool isSupportedHumidityDeviceName(const std::string &name)
{
    return name == Dht11Device::kExpectedName ||
           name == Dht11Device::kSht3xExpectedName ||
           name == "sht31";
}

// 扫描 /sys/bus/iio/devices/ 下所有 iio:deviceN，读 name 文件，
// 找到第一个 name 为 dht11/sht3x/sht31 的设备。返回其完整路径（无尾 /）。
// 找不到返回空字符串。找到多个时选第一个，不报错（调用方可记日志）。
std::string discoverDht11SysfsDir(std::string *errorMessage)
{
    DIR *dir = ::opendir(Dht11Device::kIioDevicesDir);
    if (dir == nullptr) {
        setError(errorMessage,
                 std::string("dht11 cannot open ") + Dht11Device::kIioDevicesDir +
                     ": " + std::strerror(errno));
        return std::string();
    }

    std::string found;
    int foundCount = 0;
    struct dirent *entry = nullptr;
    while ((entry = ::readdir(dir)) != nullptr) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        // IIO 设备节点形如 iio:device0, iio:device1 ...
        if (name.find("iio:device") != 0) {
            continue;
        }

        const std::string namePath =
            std::string(Dht11Device::kIioDevicesDir) + "/" + name + "/" +
            Dht11Device::kNameFile;
        std::string devName;
        std::string devNameError;
        if (!readSysfsFile(namePath, &devName, &devNameError)) {
            // 该设备 name 读不到，跳过（可能权限不足或已消失）
            continue;
        }
        if (isSupportedHumidityDeviceName(devName)) {
            if (found.empty()) {
                found = std::string(Dht11Device::kIioDevicesDir) + "/" + name;
            }
            ++foundCount;
        }
    }
    ::closedir(dir);

    if (found.empty()) {
        setError(errorMessage,
                 std::string("no supported humidity IIO device (dht11/sht3x/sht31) found in ") +
                     Dht11Device::kIioDevicesDir);
        return std::string();
    }

    if (foundCount > 1) {
        // 找到多个，选第一个，不报错。调用方可根据需要记日志。
        // errorMessage 保持空表示成功。
    }

    return found;
}

} // namespace

const char *const Dht11Device::kTempFile = "in_temp_input";
const char *const Dht11Device::kHumidityFile = "in_humidityrelative_input";
const char *const Dht11Device::kSht3xTempRawFile = "in_temp_raw";
const char *const Dht11Device::kSht3xTempScaleFile = "in_temp_scale";
const char *const Dht11Device::kSht3xHumidityRawFile = "in_humidityrelative_raw";
const char *const Dht11Device::kSht3xHumidityScaleFile = "in_humidityrelative_scale";
const char *const Dht11Device::kNameFile = "name";
const char *const Dht11Device::kExpectedName = "dht11";
const char *const Dht11Device::kSht3xExpectedName = "sht3x";
const char *const Dht11Device::kIioDevicesDir = "/sys/bus/iio/devices";

Dht11Reading::Dht11Reading()
    : valid(false),
      temperature(0.0),
      humidity(0.0),
      errorMessage()
{
}

Dht11Device::Dht11Device(const Dht11Config &config)
    : config_(config),
      available_(false),
      resolvedSysfsDir_(),
      resolvedDeviceName_()
{
}

Dht11Device::~Dht11Device()
{
    closeDevice();
}

bool Dht11Device::openDevice(std::string *errorMessage)
{
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    closeDevice();

    if (!config_.enabled) {
        return true;
    }

    // 每次重新 open 都重新解析 resolvedSysfsDir_：
    // - config_.sysfsDir 非空：直接用配置值（不修改配置）；
    // - config_.sysfsDir 为空：重新扫描 iio:device*/name=="dht11"。
    // 这样 IIO 编号在运行中改变后（拔插、driver reload），
    // closeDevice + openDevice 能自动定位到新路径，不会卡在旧路径上。
    if (config_.sysfsDir.empty()) {
        std::string discovered;
        std::string discoverError;
        discovered = discoverDht11SysfsDir(&discoverError);
        if (discovered.empty()) {
            setError(errorMessage, discoverError);
            return false;
        }
        resolvedSysfsDir_ = discovered;
    } else {
        resolvedSysfsDir_ = config_.sysfsDir;
    }

    // 读 {resolvedSysfsDir_}/name 确认设备身份，不触发实际采样。
    const std::string namePath = joinPath(resolvedSysfsDir_, kNameFile);
    std::string devName;
    std::string nameError;
    if (!readSysfsFile(namePath, &devName, &nameError)) {
        setError(errorMessage, nameError);
        return false;
    }
    if (!isSupportedHumidityDeviceName(devName)) {
        setError(errorMessage,
                 "humidity sysfs name mismatch: expected dht11/sht3x/sht31 but got \"" +
                     devName + "\" at " + resolvedSysfsDir_);
        resolvedSysfsDir_.clear();
        return false;
    }

    resolvedDeviceName_ = devName;
    available_ = true;
    return true;
}

void Dht11Device::closeDevice()
{
    available_ = false;
    // 清空 resolvedSysfsDir_：强制下次 openDevice() 重新解析路径，
    // 避免使用过期的 iio:deviceN 编号。
    resolvedSysfsDir_.clear();
    resolvedDeviceName_.clear();
}

bool Dht11Device::isAvailable() const
{
    return available_;
}

Dht11Reading Dht11Device::readTemperatureHumidity(std::string *errorMessage) const
{
    Dht11Reading reading;
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    if (!available_) {
        reading.errorMessage = "dht11 device not available";
        setError(errorMessage, reading.errorMessage);
        return reading;
    }

    std::string readError;

    if (resolvedDeviceName_ == kExpectedName) {
        std::string tempText;
        std::string humidityText;
        if (!readSysfsFile(joinPath(resolvedSysfsDir_, kTempFile),
                           &tempText, &readError) ||
            !readSysfsFile(joinPath(resolvedSysfsDir_, kHumidityFile),
                           &humidityText, &readError)) {
            reading.errorMessage = readError;
            setError(errorMessage, reading.errorMessage);
            return reading;
        }

        long tempMilli = 0;
        long humidityMilli = 0;
        if (!parseMilliValue(tempText, &tempMilli, &reading.errorMessage) ||
            !parseMilliValue(humidityText, &humidityMilli, &reading.errorMessage)) {
            setError(errorMessage, reading.errorMessage);
            return reading;
        }

        reading.temperature = static_cast<double>(tempMilli) / 1000.0;
        reading.humidity = static_cast<double>(humidityMilli) / 1000.0;
    } else {
        std::string tempRawText;
        std::string tempScaleText;
        std::string humidityRawText;
        std::string humidityScaleText;
        if (!readSysfsFile(joinPath(resolvedSysfsDir_, kSht3xTempRawFile),
                           &tempRawText, &readError) ||
            !readSysfsFile(joinPath(resolvedSysfsDir_, kSht3xTempScaleFile),
                           &tempScaleText, &readError) ||
            !readSysfsFile(joinPath(resolvedSysfsDir_, kSht3xHumidityRawFile),
                           &humidityRawText, &readError) ||
            !readSysfsFile(joinPath(resolvedSysfsDir_, kSht3xHumidityScaleFile),
                           &humidityScaleText, &readError)) {
            reading.errorMessage = readError;
            setError(errorMessage, reading.errorMessage);
            return reading;
        }

        long tempRaw = 0;
        long humidityRaw = 0;
        double tempScale = 0.0;
        double humidityScale = 0.0;
        if (!parseMilliValue(tempRawText, &tempRaw, &reading.errorMessage) ||
            !parseDoubleValue(tempScaleText, &tempScale, &reading.errorMessage) ||
            !parseMilliValue(humidityRawText, &humidityRaw, &reading.errorMessage) ||
            !parseDoubleValue(humidityScaleText, &humidityScale,
                              &reading.errorMessage)) {
            setError(errorMessage, reading.errorMessage);
            return reading;
        }

        // 本地 sht31 IIO 驱动没有导出 offset 属性：
        // temp_raw * temp_scale = 温度毫摄氏度 + 45000。
        reading.temperature =
            (static_cast<double>(tempRaw) * tempScale - 45000.0) / 1000.0;
        reading.humidity = static_cast<double>(humidityRaw) * humidityScale;
    }

    if (reading.temperature < -45.0 || reading.temperature > 130.0 ||
        reading.humidity < 0.0 || reading.humidity > 100.0) {
        reading.errorMessage = "humidity sensor reading outside physical range";
        setError(errorMessage, reading.errorMessage);
        return reading;
    }

    reading.valid = true;
    return reading;
}

} // namespace vehicledevice
