#ifndef VOICE_AUDIO_WAV_IO_H
#define VOICE_AUDIO_WAV_IO_H

/**
 * @file wav_io.h
 * @brief PCM 16-bit WAV 读写模块（纯 C++17，无外部依赖）。
 *
 * 职责分工：
 *   - writePcm16Wave：写入标准 RIFF/WAVE/fmt/data chunk，拒绝空采样/空声道/空采样率。
 *   - readPcm16Wave：解析通用 PCM16 WAV，返回采样率/声道数/位深/采样点。
 *   - checkedPcm16DataSize：纯函数，把 sampleCount*channels*sizeof(int16_t) 计算为
 *     32 位 dataSize 并做溢出保护，供写入器和单元测试复用。
 *
 * 支持：
 *   - 写入标准 RIFF/WAVE/fmt/data chunk
 *   - 读取标准 PCM（format=1）WAV
 *   - 跳过未知 chunk
 *   - 处理奇数 chunk 长度的对齐字节
 *
 * 不支持：
 *   - 非 PCM 格式（format != 1）
 *   - 非 16 位位深（调用端拒绝）
 *   - 重采样（格式不匹配时报错）
 *
 * 注意：readPcm16Wave 是通用 PCM16 解析器，不强制 16kHz/单声道。
 * 业务层（如 asr_wav_test）应根据自身需求校验 sampleRate/channels/bitsPerSample。
 */

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace voiceaudio {

struct Pcm16Wave {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    std::vector<int16_t> samples;
};

/**
 * @brief 溢出安全地计算 PCM16 WAV data chunk 的字节数。
 *
 * @param sampleCount 总采样点数（不分声道）
 * @param channels    声道数
 * @param dataSize    输出：写入字节数 = sampleCount * sizeof(int16_t)
 * @return true 计算成功且未溢出 uint32_t
 *
 * 失败条件：
 *   - dataSize == nullptr
 *   - channels == 0
 *   - sampleCount == 0（写入器语义：不允许空 WAV）
 *   - sampleCount * sizeof(int16_t) 超出 uint32_t 表示范围
 *
 * 该函数为纯函数，不访问任何缓冲区，可在不构造合法指针的情况下测试溢出路径。
 */
bool checkedPcm16DataSize(
    size_t sampleCount,
    uint16_t channels,
    uint32_t *dataSize);

/**
 * @brief 将 PCM16 数据写入标准 WAV 文件。
 *
 * @param path        输出文件路径
 * @param samples     PCM16 采样数据指针（必须非空）
 * @param sampleCount 采样点数量（必须 > 0，总采样点数不分声道）
 * @param sampleRate  采样率（必须 > 0，如 16000）
 * @param channels    声道数（必须 > 0，如 1）
 * @param errorMessage 错误信息输出（可选）
 * @return true 写入成功
 *
 * 拒绝写入空 WAV：samples==nullptr / sampleCount==0 / channels==0 / sampleRate==0
 * 均返回 false，避免生成 44 字节空文件。
 */
bool writePcm16Wave(
    const std::string &path,
    const int16_t *samples,
    size_t sampleCount,
    uint32_t sampleRate,
    uint16_t channels,
    std::string *errorMessage);

/**
 * @brief 从 WAV 文件读取 PCM16 数据（通用解析器）。
 *
 * @param path        输入文件路径
 * @param wave        输出结构体
 * @param errorMessage 错误信息输出（可选）
 * @return true 读取成功
 *
 * 注意：本函数只校验 format=1（PCM）和 bitsPerSample=16，
 * 不强制 16kHz/单声道。需要 16kHz/单声道的调用方必须自行校验
 * wave->sampleRate / wave->channels。
 */
bool readPcm16Wave(
    const std::string &path,
    Pcm16Wave *wave,
    std::string *errorMessage);

} // namespace voiceaudio

#endif // VOICE_AUDIO_WAV_IO_H
