/**
 * @file wav_io.cpp
 * @brief PCM 16-bit WAV 读写实现（纯 C++17，小端序）。
 *
 * WAV 文件格式参考：http://soundfile.sapp.org/doc/WaveFormat/
 *
 * 安全要求：
 *   - 不直接写入带填充的 C++ 结构体
 *   - 防止长度溢出
 *   - 空数据/空路径/文件写失败均安全返回 false
 *   - 读取时跳过未知 chunk，处理奇数 chunk 对齐字节
 *   - 只接受 PCM 格式（format=1）
 *   - FILE* 使用 RAII 封装，保证所有路径都关闭文件
 */

#include "wav_io.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

namespace voiceaudio {

// RAII 封装 FILE*，保证所有返回路径都关闭文件。
// 使用 unique_ptr 的自定义删除器，比手写多个 fclose 调用更安全。
using FileGuard = std::unique_ptr<FILE, decltype(&std::fclose)>;

// 小端序写入辅助
static void writeU16LE(uint8_t *buf, uint16_t val)
{
    buf[0] = static_cast<uint8_t>(val & 0xFF);
    buf[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

static void writeU32LE(uint8_t *buf, uint32_t val)
{
    buf[0] = static_cast<uint8_t>(val & 0xFF);
    buf[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    buf[2] = static_cast<uint8_t>((val >> 16) & 0xFF);
    buf[3] = static_cast<uint8_t>((val >> 24) & 0xFF);
}

// 小端序读取辅助
static uint16_t readU16LE(const uint8_t *buf)
{
    return static_cast<uint16_t>(buf[0]) |
           (static_cast<uint16_t>(buf[1]) << 8);
}

static uint32_t readU32LE(const uint8_t *buf)
{
    return static_cast<uint32_t>(buf[0]) |
           (static_cast<uint32_t>(buf[1]) << 8) |
           (static_cast<uint32_t>(buf[2]) << 16) |
           (static_cast<uint32_t>(buf[3]) << 24);
}

bool checkedPcm16DataSize(
    size_t sampleCount,
    uint16_t channels,
    uint32_t *dataSize)
{
    if (dataSize == nullptr) {
        return false;
    }
    if (channels == 0) {
        return false;
    }
    if (sampleCount == 0) {
        // 写入器语义：不允许空 WAV
        return false;
    }

    // PCM16：每个采样点 2 字节。先按 size_t 计算再校验是否超出 uint32_t 范围。
    constexpr size_t kBytesPerSample = sizeof(int16_t); // 2
    if (sampleCount > (std::numeric_limits<uint32_t>::max)() / kBytesPerSample) {
        return false; // 溢出 uint32_t
    }
    *dataSize = static_cast<uint32_t>(sampleCount * kBytesPerSample);
    return true;
}

bool writePcm16Wave(
    const std::string &path,
    const int16_t *samples,
    size_t sampleCount,
    uint32_t sampleRate,
    uint16_t channels,
    std::string *errorMessage)
{
    // 参数校验
    if (path.empty()) {
        if (errorMessage) *errorMessage = "empty path";
        return false;
    }

    if (sampleCount == 0) {
        if (errorMessage) *errorMessage = "PCM sample data is empty";
        return false;
    }

    if (samples == nullptr) {
        if (errorMessage) *errorMessage = "null samples with non-zero count";
        return false;
    }

    if (channels == 0) {
        if (errorMessage) *errorMessage = "channel count must be non-zero";
        return false;
    }

    if (sampleRate == 0) {
        if (errorMessage) *errorMessage = "sample rate must be non-zero";
        return false;
    }

    // 溢出安全的 dataSize 计算（纯函数复用，便于单元测试）
    uint32_t dataBytes = 0;
    if (!checkedPcm16DataSize(sampleCount, channels, &dataBytes)) {
        if (errorMessage) *errorMessage = "data size overflow";
        return false;
    }

    FILE *fp = std::fopen(path.c_str(), "wb");
    if (!fp) {
        if (errorMessage) *errorMessage = "cannot open file for writing: " + path;
        return false;
    }
    // RAII：guard 析构时自动 fclose，无需在每个错误分支手写 fclose
    FileGuard guard(fp, &std::fclose);

    // RIFF header
    uint8_t header[44];
    std::memset(header, 0, sizeof(header));

    const size_t bytesPerSample = sizeof(int16_t); // 2

    // "RIFF"
    header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
    // chunkSize = 36 + dataBytes
    writeU32LE(header + 4, static_cast<uint32_t>(36 + dataBytes));
    // "WAVE"
    header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';

    // "fmt "
    header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
    // subchunk1 size = 16
    writeU32LE(header + 16, 16);
    // audio format = 1 (PCM)
    writeU16LE(header + 20, 1);
    // num channels
    writeU16LE(header + 22, channels);
    // sample rate
    writeU32LE(header + 24, sampleRate);
    // byte rate = sampleRate * channels * bitsPerSample/8
    const uint32_t byteRate = sampleRate * channels * static_cast<uint16_t>(bytesPerSample);
    writeU32LE(header + 28, byteRate);
    // block align = channels * bitsPerSample/8
    const uint16_t blockAlign = static_cast<uint16_t>(channels * bytesPerSample);
    writeU16LE(header + 32, blockAlign);
    // bits per sample = 16
    writeU16LE(header + 34, 16);

    // "data"
    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    // subchunk2 size = dataBytes
    writeU32LE(header + 40, dataBytes);

    if (std::fwrite(header, 1, 44, fp) != 44) {
        if (errorMessage) *errorMessage = "failed to write header";
        // guard 析构关闭文件，再删除残留文件
        guard.reset();
        std::remove(path.c_str());
        return false;
    }

    // 写入采样数据（小端序，int16_t 在小端机器上可直接写入）
    // 检查平台端序：如果不是小端，需要逐字节转换
    // 这里假设板端（aarch64）和 x86 均为小端
    const size_t written = std::fwrite(samples, bytesPerSample, sampleCount, fp);
    if (written != sampleCount) {
        if (errorMessage) *errorMessage = "failed to write samples";
        guard.reset();
        std::remove(path.c_str());
        return false;
    }

    // guard 析构自动 fclose
    return true;
}

bool readPcm16Wave(
    const std::string &path,
    Pcm16Wave *wave,
    std::string *errorMessage)
{
    if (path.empty()) {
        if (errorMessage) *errorMessage = "empty path";
        return false;
    }

    if (!wave) {
        if (errorMessage) *errorMessage = "null output struct";
        return false;
    }

    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        if (errorMessage) *errorMessage = "cannot open file: " + path;
        return false;
    }
    // RAII：guard 析构时自动 fclose，无需在每个错误分支手写 fclose
    FileGuard guard(fp, &std::fclose);

    // 读取 RIFF header（12 字节）
    uint8_t riff[12];
    if (std::fread(riff, 1, 12, fp) != 12) {
        if (errorMessage) *errorMessage = "truncated RIFF header";
        return false;
    }

    if (std::memcmp(riff, "RIFF", 4) != 0) {
        if (errorMessage) *errorMessage = "not a RIFF file";
        return false;
    }

    if (std::memcmp(riff + 8, "WAVE", 4) != 0) {
        if (errorMessage) *errorMessage = "not a WAVE file";
        return false;
    }

    bool fmtFound = false;
    bool dataFound = false;
    uint16_t audioFormat = 0;
    uint16_t numChannels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    uint32_t dataSize = 0;
    std::vector<int16_t> samples;

    // 遍历 chunks
    while (!fmtFound || !dataFound) {
        uint8_t chunkHeader[8];
        size_t rd = std::fread(chunkHeader, 1, 8, fp);
        if (rd != 8) {
            if (errorMessage) {
                *errorMessage = fmtFound
                    ? "missing data chunk"
                    : "missing fmt chunk or unexpected EOF";
            }
            return false;
        }

        uint32_t chunkSize = readU32LE(chunkHeader + 4);

        if (std::memcmp(chunkHeader, "fmt ", 4) == 0) {
            if (chunkSize < 16 || chunkSize > 1024) {
                if (errorMessage) *errorMessage = "invalid fmt chunk size";
                return false;
            }

            uint8_t fmt[16];
            if (std::fread(fmt, 1, 16, fp) != 16) {
                if (errorMessage) *errorMessage = "truncated fmt chunk";
                return false;
            }

            audioFormat = readU16LE(fmt);
            numChannels = readU16LE(fmt + 2);
            sampleRate = readU32LE(fmt + 4);
            bitsPerSample = readU16LE(fmt + 14);

            // 跳过 fmt chunk 剩余字节
            if (chunkSize > 16) {
                uint32_t extra = chunkSize - 16;
                if (std::fseek(fp, static_cast<long>(extra), SEEK_CUR) != 0) {
                    if (errorMessage) *errorMessage = "failed to skip fmt extra";
                    return false;
                }
            }

            // fmt chunk 奇数对齐
            if (chunkSize % 2 != 0) {
                std::fgetc(fp); // 跳过 1 字节 padding
            }

            fmtFound = true;
        } else if (std::memcmp(chunkHeader, "data", 4) == 0) {
            dataSize = chunkSize;

            // 溢出检查
            if (dataSize > std::numeric_limits<uint32_t>::max() - 8) {
                if (errorMessage) *errorMessage = "data size overflow";
                return false;
            }

            if (bitsPerSample == 0) {
                // fmt 还没读到或者 bitsPerSample 未设置
                // 此时无法确定采样数，报错
                if (errorMessage) *errorMessage = "data chunk before fmt chunk";
                return false;
            }

            if (bitsPerSample != 16) {
                if (errorMessage) {
                    *errorMessage = "unsupported bits per sample: " +
                                    std::to_string(bitsPerSample) + " (only 16 supported)";
                }
                return false;
            }

            if (dataSize % 2 != 0) {
                if (errorMessage) *errorMessage = "odd data size for 16-bit PCM";
                return false;
            }

            size_t numSamples = dataSize / sizeof(int16_t);
            if (numSamples == 0) {
                if (errorMessage) *errorMessage = "empty data chunk";
                return false;
            }

            samples.resize(numSamples);
            if (std::fread(samples.data(), sizeof(int16_t), numSamples, fp) != numSamples) {
                if (errorMessage) *errorMessage = "truncated data chunk";
                return false;
            }

            // data chunk 奇数对齐
            if (dataSize % 2 != 0) {
                std::fgetc(fp);
            }

            dataFound = true;
        } else {
            // 未知 chunk，跳过
            if (chunkSize > 0) {
                if (std::fseek(fp, static_cast<long>(chunkSize), SEEK_CUR) != 0) {
                    if (errorMessage) *errorMessage = "failed to skip unknown chunk";
                    return false;
                }
            }
            if (chunkSize % 2 != 0) {
                std::fgetc(fp);
            }
        }
    }

    // guard 析构自动 fclose

    // 格式校验
    if (audioFormat != 1) {
        if (errorMessage) {
            *errorMessage = "non-PCM format: " + std::to_string(audioFormat);
        }
        return false;
    }

    wave->sampleRate = sampleRate;
    wave->channels = numChannels;
    wave->bitsPerSample = bitsPerSample;
    wave->samples = std::move(samples);

    return true;
}

} // namespace voiceaudio
