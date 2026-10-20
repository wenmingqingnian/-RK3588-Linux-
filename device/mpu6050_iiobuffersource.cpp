#include "mpu6050_iiobuffersource.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

namespace vehicledevice {
namespace {

constexpr double kGravity = 9.80665;

void setError(std::string *errorMessage, const std::string &message)
{
    if (errorMessage != nullptr)
        *errorMessage = message;
}

std::string iioError(const std::string &prefix)
{
    return prefix + ": " + std::strerror(errno);
}

} // namespace

Mpu6050IioBufferSource::Mpu6050IioBufferSource(const Mpu6050Config &config)
    : config_(config),
      fd_(-1),
      sampleSize_(0),
      tempChanOffset_(-1),
      timestampOffset_(-1),
      tempBigEndian_(false),
      accelScale_(0.0),
      gyroScale_(0.0),
      tempScale_(0.0),
      tempOffset_(0)
{
    for (int i = 0; i < 3; ++i) {
        accelChans_[i] = -1;
        gyroChans_[i] = -1;
        accelBigEndian_[i] = false;
        gyroBigEndian_[i] = false;
    }
}

Mpu6050IioBufferSource::~Mpu6050IioBufferSource()
{
    close();
}

std::string Mpu6050IioBufferSource::findMpu6500Device() const
{
    const std::string dirPath = "/sys/bus/iio/devices";
    DIR *dir = ::opendir(dirPath.c_str());
    if (!dir)
        return std::string();

    std::string result;
    struct dirent *ent;
    while ((ent = ::readdir(dir)) != nullptr) {
        if (std::strncmp(ent->d_name, "iio:device", 10) != 0)
            continue;

        const std::string namePath = dirPath + "/" + ent->d_name + "/name";
        std::ifstream f(namePath);
        std::string name;
        if (std::getline(f, name) && name == "mpu6500") {
            result = dirPath + "/" + ent->d_name;
            break;
        }
    }
    ::closedir(dir);
    return result;
}

bool Mpu6050IioBufferSource::readSysfsInt(const std::string &path, int *value) const
{
    std::ifstream f(path);
    if (!f)
        return false;
    *value = 0;
    f >> *value;
    return f.good() || f.eof();
}

bool Mpu6050IioBufferSource::readSysfsDouble(const std::string &path, double *value) const
{
    std::ifstream f(path);
    if (!f)
        return false;
    *value = 0.0;
    f >> *value;
    return f.good() || f.eof();
}

bool Mpu6050IioBufferSource::writeSysfs(const std::string &path, const std::string &value) const
{
    std::ofstream f(path);
    if (!f)
        return false;
    f << value;
    return f.good();
}

bool Mpu6050IioBufferSource::parseScanLayout(std::string *errorMessage)
{
    // 扫描 scan_elements，根据 index 排序确定样本内偏移
    // IIO 样本布局: 按index 升序排列的通道，每个通道按其 type 中的 size 对齐。
    // MPU6500 的 channel: accel_x/y/z, anglvel_x/y/z, temp, timestamp
    // 每个数据通道默认 2 字节（le:s16/16），timestamp 是 8 字节（le:s64/64）。

    struct Channel {
        std::string name;
        int index;
        int size;       // 字节
        int offset;     // 在样本中的字节偏移（计算后）
        bool bigEndian;
    };

    sampleSize_ = 0;
    tempChanOffset_ = -1;
    timestampOffset_ = -1;
    tempBigEndian_ = false;
    for (int i = 0; i < 3; ++i) {
        accelChans_[i] = -1;
        gyroChans_[i] = -1;
        accelBigEndian_[i] = false;
        gyroBigEndian_[i] = false;
    }

    const std::vector<std::string> channelNames = {
        "in_accel_x", "in_accel_y", "in_accel_z",
        "in_anglvel_x", "in_anglvel_y", "in_anglvel_z",
        "in_temp", "in_timestamp"
    };

    std::vector<Channel> chans;
    for (const auto &base : channelNames) {
        const std::string enPath = sysfsPath_ + "/scan_elements/" + base + "_en";
        int en = 0;
        if (!readSysfsInt(enPath, &en) || en != 1)
            continue;  // 通道未启用

        Channel ch;
        ch.name = base;

        const std::string idxPath = sysfsPath_ + "/scan_elements/" + base + "_index";
        if (!readSysfsInt(idxPath, &ch.index)) {
            setError(errorMessage, "failed to read index for " + base);
            return false;
        }

        // 读取 type: 格式 "le:s16/16>>0" 或 "le:s64/64>>0"
        const std::string typePath = sysfsPath_ + "/scan_elements/" + base + "_type";
        std::ifstream tf(typePath);
        std::string typeStr;
        if (!std::getline(tf, typeStr)) {
            setError(errorMessage, "failed to read type for " + base);
            return false;
        }
        if (typeStr.rfind("be:", 0) == 0)
            ch.bigEndian = true;
        else if (typeStr.rfind("le:", 0) == 0)
            ch.bigEndian = false;
        else {
            setError(errorMessage, "unsupported IIO endian type: " + typeStr);
            return false;
        }

        // 解析 storage bits（斜杠后的数值；例如 s16/16>>0）。
        auto slash = typeStr.find('/');
        if (slash == std::string::npos) {
            setError(errorMessage, "invalid type format: " + typeStr);
            return false;
        }
        int storageBits = 0;
        std::istringstream(typeStr.substr(slash + 1)) >> storageBits;
        if (storageBits <= 0 || storageBits % 8 != 0) {
            setError(errorMessage, "invalid storage size: " + typeStr);
            return false;
        }
        ch.size = storageBits / 8;

        if (base != "in_timestamp" && ch.size != 2) {
            setError(errorMessage, "unsupported sensor channel type: " + typeStr);
            return false;
        }

        chans.push_back(ch);
    }

    if (chans.empty()) {
        setError(errorMessage, "no scan_elements enabled");
        return false;
    }

    // 按 index 升序排列
    std::sort(chans.begin(), chans.end(),
              [](const Channel &a, const Channel &b) { return a.index < b.index; });

    // 计算偏移（IIO 默认布局：自然对齐到通道 size）
    int offset = 0;
    for (auto &ch : chans) {
        // 对齐到 ch.size 边界
        if (offset % ch.size != 0)
            offset = (offset / ch.size + 1) * ch.size;
        ch.offset = offset;
        offset += ch.size;
    }
    sampleSize_ = offset;

    // 填充到成员变量
    for (const auto &ch : chans) {
        if (ch.name == "in_accel_x") {
            accelChans_[0] = ch.offset; accelBigEndian_[0] = ch.bigEndian;
        } else if (ch.name == "in_accel_y") {
            accelChans_[1] = ch.offset; accelBigEndian_[1] = ch.bigEndian;
        } else if (ch.name == "in_accel_z") {
            accelChans_[2] = ch.offset; accelBigEndian_[2] = ch.bigEndian;
        } else if (ch.name == "in_anglvel_x") {
            gyroChans_[0] = ch.offset; gyroBigEndian_[0] = ch.bigEndian;
        } else if (ch.name == "in_anglvel_y") {
            gyroChans_[1] = ch.offset; gyroBigEndian_[1] = ch.bigEndian;
        } else if (ch.name == "in_anglvel_z") {
            gyroChans_[2] = ch.offset; gyroBigEndian_[2] = ch.bigEndian;
        } else if (ch.name == "in_temp") {
            tempChanOffset_ = ch.offset; tempBigEndian_ = ch.bigEndian;
        } else if (ch.name == "in_timestamp") {
            timestampOffset_ = ch.offset;
        }
    }

    for (int i = 0; i < 3; ++i) {
        if (accelChans_[i] < 0 || gyroChans_[i] < 0) {
            setError(errorMessage, "required MPU6500 accel/gyro scan channel missing");
            return false;
        }
    }
    if (tempChanOffset_ < 0) {
        setError(errorMessage, "required MPU6500 temperature scan channel missing");
        return false;
    }

    return true;
}

bool Mpu6050IioBufferSource::enableBuffer(std::string *errorMessage)
{
    // 必须先关闭 buffer，IIO ABI 不允许在 buffer 启用时修改 scan_elements。
    if (!writeSysfs(sysfsPath_ + "/buffer/enable", "0")) {
        setError(errorMessage, "failed to disable buffer before configuration");
        return false;
    }

    // 1. 启用所有 scan_elements
    const std::vector<std::string> channels = {
        "in_accel_x", "in_accel_y", "in_accel_z",
        "in_anglvel_x", "in_anglvel_y", "in_anglvel_z",
        "in_temp", "in_timestamp"
    };
    for (const auto &ch : channels) {
        const std::string path = sysfsPath_ + "/scan_elements/" + ch + "_en";
        if (!writeSysfs(path, "1")) {
            setError(errorMessage, "failed to enable " + ch + ": " + iioError(""));
            return false;
        }
    }

    // 2. 设置 buffer 长度（容纳 100 个样本）
    if (!writeSysfs(sysfsPath_ + "/buffer/length", "100")) {
        setError(errorMessage, "failed to set buffer length");
        return false;
    }

    // 3. 启用 buffer
    if (!writeSysfs(sysfsPath_ + "/buffer/enable", "1")) {
        setError(errorMessage, "failed to enable buffer");
        return false;
    }

    return true;
}

void Mpu6050IioBufferSource::disableBuffer()
{
    writeSysfs(sysfsPath_ + "/buffer/enable", "0");
}

bool Mpu6050IioBufferSource::open(std::string *errorMessage)
{
    if (errorMessage)
        errorMessage->clear();

    close();

    if (!config_.enabled)
        return true;

    // 1. 查找 mpu6500 IIO 设备
    sysfsPath_ = findMpu6500Device();
    if (sysfsPath_.empty()) {
        setError(errorMessage, "mpu6500 IIO device not found");
        return false;
    }

    // 2. 读取 scale（IIO ABI: 实际值 = raw * scale）
    double accelScaleRaw = 0.0, gyroScaleRaw = 0.0, tempScaleRaw = 0.0;
    if (!readSysfsDouble(sysfsPath_ + "/in_accel_scale", &accelScaleRaw)) {
        setError(errorMessage, "failed to read in_accel_scale");
        return false;
    }
    if (!readSysfsDouble(sysfsPath_ + "/in_anglvel_scale", &gyroScaleRaw)) {
        setError(errorMessage, "failed to read in_anglvel_scale");
        return false;
    }
    if (!readSysfsDouble(sysfsPath_ + "/in_temp_scale", &tempScaleRaw)) {
        setError(errorMessage, "failed to read in_temp_scale");
        return false;
    }

    // IIO scale 默认是 m/s² per LSB（accel）和 rad/s per LSB（anglvel）
    // 需要把 rad/s 转成 deg/s
    accelScale_ = accelScaleRaw;             // m/s^2 per LSB
    gyroScale_ = gyroScaleRaw * 180.0 / M_PI; // deg/s per LSB
    tempScale_ = tempScaleRaw;

    // 温度 offset（IIO ABI: temp = (raw + offset) * scale）
    int tempOff = 0;
    if (readSysfsInt(sysfsPath_ + "/in_temp_offset", &tempOff))
        tempOffset_ = tempOff;
    else
        tempOffset_ = 0;

    // 3. 先启用 scan_elements 和 buffer；原实现先解析 _en，
    //    在全新启动的 _en=0 状态下必然得到 no scan_elements enabled。
    if (!enableBuffer(errorMessage)) {
        disableBuffer();
        return false;
    }

    // 4. 在通道已启用后解析扫描布局。
    if (!parseScanLayout(errorMessage)) {
        disableBuffer();
        return false;
    }

    // 5. 分配样本缓冲区
    sampleBuf_.resize(sampleSize_);

    // 6. 打开字符设备
    // 从 sysfsPath_ 提取设备号: iio:deviceN → /dev/iio:deviceN
    auto pos = sysfsPath_.rfind("iio:device");
    std::string devName = (pos != std::string::npos) ? sysfsPath_.substr(pos) : "iio:device0";
    const std::string devPath = "/dev/" + devName;

    fd_ = ::open(devPath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) {
        setError(errorMessage, "failed to open " + devPath + ": " + iioError(""));
        disableBuffer();
        return false;
    }

    return true;
}

void Mpu6050IioBufferSource::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (!sysfsPath_.empty())
        disableBuffer();
    sysfsPath_.clear();
    sampleSize_ = 0;
}

Mpu6050Reading Mpu6050IioBufferSource::readSample(std::string *errorMessage)
{
    Mpu6050Reading reading;
    if (errorMessage)
        errorMessage->clear();

    if (fd_ < 0) {
        reading.errorMessage = "iio buffer source not open";
        setError(errorMessage, reading.errorMessage);
        return reading;
    }

    // 读一个样本（sampleSize_ 字节）
    ssize_t total = 0;
    while (total < sampleSize_) {
        ssize_t n = ::read(fd_, sampleBuf_.data() + total, sampleSize_ - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            reading.errorMessage = "iio read failed: " + iioError("");
            setError(errorMessage, reading.errorMessage);
            return reading;
        }
        if (n == 0) {
            reading.errorMessage = "iio read EOF";
            setError(errorMessage, reading.errorMessage);
            return reading;
        }
        total += n;
    }

    auto readS16 = [this](int offset, bool bigEndian) -> int16_t {
        if (offset < 0 || offset + 2 > sampleSize_)
            return 0;
        const uint16_t first = static_cast<uint16_t>(sampleBuf_[offset]);
        const uint16_t second = static_cast<uint16_t>(sampleBuf_[offset + 1]);
        const uint16_t raw = bigEndian ?
            static_cast<uint16_t>((first << 8) | second) :
            static_cast<uint16_t>(first | (second << 8));
        return static_cast<int16_t>(raw);
    };

    // 加速度（m/s^2）
    if (accelChans_[0] >= 0) reading.accelX = readS16(accelChans_[0], accelBigEndian_[0]) * accelScale_;
    if (accelChans_[1] >= 0) reading.accelY = readS16(accelChans_[1], accelBigEndian_[1]) * accelScale_;
    if (accelChans_[2] >= 0) reading.accelZ = readS16(accelChans_[2], accelBigEndian_[2]) * accelScale_;

    // 陀螺仪（deg/s）
    if (gyroChans_[0] >= 0) reading.gyroX = readS16(gyroChans_[0], gyroBigEndian_[0]) * gyroScale_;
    if (gyroChans_[1] >= 0) reading.gyroY = readS16(gyroChans_[1], gyroBigEndian_[1]) * gyroScale_;
    if (gyroChans_[2] >= 0) reading.gyroZ = readS16(gyroChans_[2], gyroBigEndian_[2]) * gyroScale_;

    // 该 inv_mpu IIO 驱动的温度 scale 单位是毫摄氏度/LSB。
    if (tempChanOffset_ >= 0) {
        const int16_t tempRaw = readS16(tempChanOffset_, tempBigEndian_);
        reading.temperature =
            (static_cast<double>(tempRaw) + tempOffset_) * tempScale_ / 1000.0;
    }

    reading.valid = true;
    return reading;
}

} // namespace vehicledevice
