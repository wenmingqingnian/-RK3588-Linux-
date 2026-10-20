/**
 * @file test_wav_io.cpp
 * @brief wav_io 模块单元测试（纯 C++17，无外部依赖）。
 *
 * 覆盖：
 *   1. 写入空路径失败
 *   2. 写入空采样失败（sampleCount=0 不生成空 WAV）
 *   3. 生成 16kHz 单声道 PCM16 WAV
 *   4. 回读后元数据相同
 *   5. 回读后采样点逐个相同
 *   6. 截断 RIFF 头失败
 *   7. 非 WAVE 文件失败
 *   8. 非 PCM 格式失败
 *   9. 非 16 位格式由调用端拒绝
 *  10. 未知 chunk 可以跳过
 *  11. 奇数 chunk 长度正确跳过 padding
 *  12. dataSize 越界失败
 *  13. checkedPcm16DataSize 正常输入计算正确
 *  14. checkedPcm16DataSize 空采样/空声道/超大输入/空输出指针 全部拒绝
 *  15. samples=nullptr + 非零 sampleCount 失败
 */

#include "../audio/wav_io.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

static int g_passCount = 0;
static int g_failCount = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        ++g_passCount; \
    } else { \
        ++g_failCount; \
        std::cerr << "FAIL: " << msg << std::endl; \
    } \
} while(0)

// 辅助：生成临时文件路径
static std::string tempFile(const char *suffix)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf), "/tmp/wav_io_test_%s", suffix);
    return std::string(buf);
}

// 辅助：删除文件
static void removeFile(const std::string &path)
{
    std::remove(path.c_str());
}

// 辅助：检查文件是否存在
static bool fileExists(const std::string &path)
{
    std::ifstream f(path);
    return f.good();
}

// 辅助：生成测试采样数据
static std::vector<int16_t> makeTestSamples(size_t count)
{
    std::vector<int16_t> samples(count);
    for (size_t i = 0; i < count; ++i) {
        samples[i] = static_cast<int16_t>((i * 37) % 65536 - 32768);
    }
    return samples;
}

// 测试 1: 写入空路径失败
static void testWriteEmptyPath()
{
    std::string err;
    int16_t dummy = 0;
    bool ok = voiceaudio::writePcm16Wave("", &dummy, 1, 16000, 1, &err);
    CHECK(!ok, "test 1: empty path should fail");
    CHECK(!err.empty(), "test 1: error message should not be empty");
}

// 测试 2: 写入空采样失败（sampleCount=0 拒绝生成 44 字节空 WAV）
static void testWriteEmptySamples()
{
    std::string path = tempFile("empty.wav");
    std::string err;
    int16_t dummy = 0;
    // sampleCount=0 必须拒绝，且不生成文件
    bool ok = voiceaudio::writePcm16Wave(path, &dummy, 0, 16000, 1, &err);
    CHECK(!ok, "test 2: sampleCount=0 should fail");
    CHECK(!err.empty(), "test 2: error message should not be empty");
    CHECK(!fileExists(path), "test 2: should not create empty 44-byte WAV");
    removeFile(path);
}

// 测试 3: 生成 16kHz 单声道 PCM16 WAV
static void testWriteValidWav()
{
    std::string path = tempFile("valid.wav");
    auto samples = makeTestSamples(1600); // 100ms @ 16kHz
    std::string err;
    bool ok = voiceaudio::writePcm16Wave(path, samples.data(), samples.size(),
                                         16000, 1, &err);
    CHECK(ok, "test 3: should write valid WAV: " + err);

    // 检查文件大小
    FILE *fp = std::fopen(path.c_str(), "rb");
    CHECK(fp != nullptr, "test 3: file should exist");
    if (fp) {
        std::fseek(fp, 0, SEEK_END);
        long size = std::ftell(fp);
        std::fclose(fp);
        // 44 header + 1600*2 = 3244
        CHECK(size == 44 + 1600 * 2, "test 3: file size should be 3244");
    }
    removeFile(path);
}

// 测试 4 & 5: 回读后元数据相同 + 采样点逐个相同
static void testReadBack()
{
    std::string path = tempFile("readback.wav");
    auto original = makeTestSamples(3200); // 200ms @ 16kHz
    std::string err;

    bool ok = voiceaudio::writePcm16Wave(path, original.data(), original.size(),
                                         16000, 1, &err);
    CHECK(ok, "test 4/5: write should succeed: " + err);

    voiceaudio::Pcm16Wave wave;
    ok = voiceaudio::readPcm16Wave(path, &wave, &err);
    CHECK(ok, "test 4/5: read should succeed: " + err);

    // 测试 4: 元数据相同
    CHECK(wave.sampleRate == 16000, "test 4: sampleRate should be 16000");
    CHECK(wave.channels == 1, "test 4: channels should be 1");
    CHECK(wave.bitsPerSample == 16, "test 4: bitsPerSample should be 16");

    // 测试 5: 采样点逐个相同
    CHECK(wave.samples.size() == original.size(), "test 5: sample count mismatch");
    if (wave.samples.size() == original.size()) {
        bool allMatch = true;
        for (size_t i = 0; i < original.size(); ++i) {
            if (wave.samples[i] != original[i]) {
                allMatch = false;
                break;
            }
        }
        CHECK(allMatch, "test 5: all samples should match");
    }
    removeFile(path);
}

// 测试 6: 截断 RIFF 头失败
static void testTruncatedRiff()
{
    std::string path = tempFile("trunc.wav");
    // 写入只有 5 字节的文件
    {
        std::ofstream ofs(path, std::ios::binary);
        const char data[] = "RIFF";
        ofs.write(data, 4);
    }

    voiceaudio::Pcm16Wave wave;
    std::string err;
    bool ok = voiceaudio::readPcm16Wave(path, &wave, &err);
    CHECK(!ok, "test 6: truncated RIFF should fail");
    removeFile(path);
}

// 测试 7: 非 WAVE 文件失败
static void testNonWave()
{
    std::string path = tempFile("nonwave.wav");
    // 写入 RIFF 但不是 WAVE
    {
        std::ofstream ofs(path, std::ios::binary);
        const char data[] = "RIFF\x00\x00\x00\x00XXXX";
        ofs.write(data, 12);
    }

    voiceaudio::Pcm16Wave wave;
    std::string err;
    bool ok = voiceaudio::readPcm16Wave(path, &wave, &err);
    CHECK(!ok, "test 7: non-WAVE should fail");
    removeFile(path);
}

// 测试 8: 非 PCM 格式失败
static void testNonPcm()
{
    std::string path = tempFile("nonpcm.wav");
    // 手动构造 format=3 (IEEE float) 的 WAV
    {
        std::ofstream ofs(path, std::ios::binary);
        uint8_t header[44];
        std::memset(header, 0, sizeof(header));
        header[0]='R'; header[1]='I'; header[2]='F'; header[3]='F';
        header[8]='W'; header[9]='A'; header[10]='V'; header[11]='E';
        header[12]='f'; header[13]='m'; header[14]='t'; header[15]=' ';
        // fmt size = 16
        header[16]=16; header[17]=0; header[18]=0; header[19]=0;
        // format = 3 (non-PCM)
        header[20]=3; header[21]=0;
        // channels = 1
        header[22]=1; header[23]=0;
        // sample rate = 16000
        header[24]=0x80; header[25]=0x3E; header[26]=0; header[27]=0;
        // byte rate
        header[28]=0; header[29]=0x7D; header[30]=0; header[31]=0;
        // block align
        header[32]=2; header[33]=0;
        // bits per sample = 16
        header[34]=16; header[35]=0;
        // data chunk
        header[36]='d'; header[37]='a'; header[38]='t'; header[39]='a';
        // data size = 4
        header[40]=4; header[41]=0; header[42]=0; header[43]=0;
        ofs.write(reinterpret_cast<const char*>(header), 44);
        int16_t dummy = 0;
        ofs.write(reinterpret_cast<const char*>(&dummy), 2);
        ofs.write(reinterpret_cast<const char*>(&dummy), 2);
    }

    voiceaudio::Pcm16Wave wave;
    std::string err;
    bool ok = voiceaudio::readPcm16Wave(path, &wave, &err);
    CHECK(!ok, "test 8: non-PCM should fail");
    removeFile(path);
}

// 测试 9: 非 16 位格式由调用端拒绝
static void testNon16Bit()
{
    std::string path = tempFile("non16.wav");
    // 手动构造 8-bit PCM WAV
    {
        std::ofstream ofs(path, std::ios::binary);
        uint8_t header[44];
        std::memset(header, 0, sizeof(header));
        header[0]='R'; header[1]='I'; header[2]='F'; header[3]='F';
        header[8]='W'; header[9]='A'; header[10]='V'; header[11]='E';
        header[12]='f'; header[13]='m'; header[14]='t'; header[15]=' ';
        header[16]=16; header[17]=0; header[18]=0; header[19]=0;
        header[20]=1; header[21]=0; // PCM
        header[22]=1; header[23]=0; // mono
        header[24]=0x80; header[25]=0x3E; header[26]=0; header[27]=0; // 16000
        header[28]=0x80; header[29]=0x3E; header[30]=0; header[31]=0;
        header[32]=1; header[33]=0; // block align
        header[34]=8; header[35]=0; // 8-bit
        header[36]='d'; header[37]='a'; header[38]='t'; header[39]='a';
        header[40]=2; header[41]=0; header[42]=0; header[43]=0;
        ofs.write(reinterpret_cast<const char*>(header), 44);
        uint8_t dummy[2] = {0, 0};
        ofs.write(reinterpret_cast<const char*>(dummy), 2);
    }

    voiceaudio::Pcm16Wave wave;
    std::string err;
    bool ok = voiceaudio::readPcm16Wave(path, &wave, &err);
    CHECK(!ok, "test 9: 8-bit should be rejected");
    removeFile(path);
}

// 测试 10 & 11: 未知 chunk 跳过 + 奇数 chunk padding
static void testUnknownChunkAndPadding()
{
    std::string path = tempFile("unknown.wav");
    // 构造带未知 chunk（LIST，奇数长度 3）的 WAV
    {
        std::ofstream ofs(path, std::ios::binary);

        // RIFF header
        const char riff[] = "RIFF";
        ofs.write(riff, 4);
        uint32_t chunkSize = 12 + 8+16 + 8+3+1 + 8+4; // riff + fmt + unknown(padding) + data
        ofs.write(reinterpret_cast<const char*>(&chunkSize), 4);
        const char wave[] = "WAVE";
        ofs.write(wave, 4);

        // fmt chunk
        const char fmtId[] = "fmt ";
        ofs.write(fmtId, 4);
        uint32_t fmtSize = 16;
        ofs.write(reinterpret_cast<const char*>(&fmtSize), 4);
        uint16_t audioFormat = 1;
        ofs.write(reinterpret_cast<const char*>(&audioFormat), 2);
        uint16_t channels = 1;
        ofs.write(reinterpret_cast<const char*>(&channels), 2);
        uint32_t sampleRate = 16000;
        ofs.write(reinterpret_cast<const char*>(&sampleRate), 4);
        uint32_t byteRate = 32000;
        ofs.write(reinterpret_cast<const char*>(&byteRate), 4);
        uint16_t blockAlign = 2;
        ofs.write(reinterpret_cast<const char*>(&blockAlign), 2);
        uint16_t bitsPerSample = 16;
        ofs.write(reinterpret_cast<const char*>(&bitsPerSample), 2);

        // unknown chunk "LIST" with 3 bytes data (odd) + 1 padding
        const char listId[] = "LIST";
        ofs.write(listId, 4);
        uint32_t listSize = 3;
        ofs.write(reinterpret_cast<const char*>(&listSize), 4);
        const char listData[] = "abc";
        ofs.write(listData, 3);
        char padding = 0;
        ofs.write(&padding, 1);

        // data chunk
        const char dataId[] = "data";
        ofs.write(dataId, 4);
        uint32_t dataSize = 4; // 2 samples
        ofs.write(reinterpret_cast<const char*>(&dataSize), 4);
        int16_t s1 = 100;
        int16_t s2 = -200;
        ofs.write(reinterpret_cast<const char*>(&s1), 2);
        ofs.write(reinterpret_cast<const char*>(&s2), 2);
    }

    voiceaudio::Pcm16Wave wave;
    std::string err;
    bool ok = voiceaudio::readPcm16Wave(path, &wave, &err);
    CHECK(ok, "test 10/11: should skip unknown chunk: " + err);
    if (ok) {
        CHECK(wave.samples.size() == 2, "test 10: should have 2 samples");
        if (wave.samples.size() == 2) {
            CHECK(wave.samples[0] == 100, "test 10: sample 0 should be 100");
            CHECK(wave.samples[1] == -200, "test 10: sample 1 should be -200");
        }
    }
    removeFile(path);
}

// 测试 12: dataSize 越界失败
static void testDataSizeOverflow()
{
    std::string path = tempFile("overflow.wav");
    // 构造 dataSize 声称很大但文件实际很小
    {
        std::ofstream ofs(path, std::ios::binary);
        uint8_t header[44];
        std::memset(header, 0, sizeof(header));
        header[0]='R'; header[1]='I'; header[2]='F'; header[3]='F';
        header[8]='W'; header[9]='A'; header[10]='V'; header[11]='E';
        header[12]='f'; header[13]='m'; header[14]='t'; header[15]=' ';
        header[16]=16; header[17]=0; header[18]=0; header[19]=0;
        header[20]=1; header[21]=0; // PCM
        header[22]=1; header[23]=0; // mono
        header[24]=0x80; header[25]=0x3E; header[26]=0; header[27]=0; // 16000
        header[28]=0; header[29]=0x7D; header[30]=0; header[31]=0;
        header[32]=2; header[33]=0;
        header[34]=16; header[35]=0;
        header[36]='d'; header[37]='a'; header[38]='t'; header[39]='a';
        // dataSize = 0xFFFFFFFF（超大）
        header[40]=0xFF; header[41]=0xFF; header[42]=0xFF; header[43]=0xFF;
        ofs.write(reinterpret_cast<const char*>(header), 44);
    }

    voiceaudio::Pcm16Wave wave;
    std::string err;
    bool ok = voiceaudio::readPcm16Wave(path, &wave, &err);
    CHECK(!ok, "test 12: oversized dataSize should fail");
    removeFile(path);
}

// 测试 13: checkedPcm16DataSize 正常输入计算正确
// 该测试为纯函数测试，不写文件、不访问任何缓冲区，
// 避免 nullptr + 巨大 count 在写入器里被空指针检查先拦截。
static void testCheckedPcm16DataSizeNormal()
{
    uint32_t dataSize = 0;
    bool ok = voiceaudio::checkedPcm16DataSize(16000, 1, &dataSize);
    CHECK(ok, "test 13: 16000 samples x 1 channel should succeed");
    CHECK(dataSize == 32000, "test 13: dataSize should be 32000");

    // 多声道同样按总采样点数*2 字节计算（writePcm16Wave 用 sampleCount 表示总采样点数）
    uint32_t dataSize2 = 0;
    bool ok2 = voiceaudio::checkedPcm16DataSize(8000, 2, &dataSize2);
    CHECK(ok2, "test 13b: 8000 samples x 2 channels should succeed");
    CHECK(dataSize2 == 16000, "test 13b: dataSize should be 16000");
}

// 测试 14: checkedPcm16DataSize 全部失败路径
static void testCheckedPcm16DataSizeFailures()
{
    // 0 samples → false（写入器语义：不允许空 WAV）
    uint32_t dataSize = 0;
    CHECK(!voiceaudio::checkedPcm16DataSize(0, 1, &dataSize),
          "test 14a: 0 samples should fail");

    // 0 channels → false
    CHECK(!voiceaudio::checkedPcm16DataSize(100, 0, &dataSize),
          "test 14b: 0 channels should fail");

    // dataSize=nullptr → false
    CHECK(!voiceaudio::checkedPcm16DataSize(100, 1, nullptr),
          "test 14c: nullptr output should fail");

    // 超大 sampleCount → false（溢出 uint32_t）
    // sampleCount > UINT32_MAX / 2 即溢出
    size_t hugeCount = static_cast<size_t>(std::numeric_limits<uint32_t>::max()) / 2 + 1;
    CHECK(!voiceaudio::checkedPcm16DataSize(hugeCount, 1, &dataSize),
          "test 14d: overflow sampleCount should fail");

    // 刚好不溢出的边界：UINT32_MAX/2 应成功
    size_t boundaryCount = static_cast<size_t>(std::numeric_limits<uint32_t>::max()) / 2;
    uint32_t boundaryDataSize = 0;
    bool boundaryOk = voiceaudio::checkedPcm16DataSize(boundaryCount, 1, &boundaryDataSize);
    CHECK(boundaryOk, "test 14e: boundary count should succeed");
    CHECK(boundaryDataSize == std::numeric_limits<uint32_t>::max() - 1,
          "test 14e: boundary dataSize should be UINT32_MAX-1");
}

// 测试 15: samples=nullptr + 非零 sampleCount 失败（写入器路径）
static void testWriteNullSamples()
{
    std::string path = tempFile("null_samples.wav");
    std::string err;
    bool ok = voiceaudio::writePcm16Wave(path, nullptr, 100, 16000, 1, &err);
    CHECK(!ok, "test 15: null samples with non-zero count should fail");
    CHECK(!fileExists(path), "test 15: should not create file");
    removeFile(path);
}

int main()
{
    testWriteEmptyPath();            // 1
    testWriteEmptySamples();         // 2
    testWriteValidWav();             // 3
    testReadBack();                  // 4, 5
    testTruncatedRiff();             // 6
    testNonWave();                   // 7
    testNonPcm();                    // 8
    testNon16Bit();                  // 9
    testUnknownChunkAndPadding();    // 10, 11
    testDataSizeOverflow();          // 12
    testCheckedPcm16DataSizeNormal();// 13 (含 13b)
    testCheckedPcm16DataSizeFailures();// 14 (a-e)
    testWriteNullSamples();          // 15

    std::cout << "wav_io tests: " << g_passCount << " passed, "
              << g_failCount << " failed" << std::endl;

    return g_failCount > 0 ? 1 : 0;
}
