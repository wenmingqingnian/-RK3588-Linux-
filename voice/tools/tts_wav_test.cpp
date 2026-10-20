/**
 * @file tts_wav_test.cpp
 * @brief TTS→WAV 命令行工具。
 *
 * 使用 SummerTTS 引擎合成中文语音，写入标准 16kHz/单声道/PCM16 WAV 文件。
 * 不创建 AudioPlayer，不调用 ALSA，不创建 ZMQ，不启动线程队列。
 *
 * 用法：
 *   tts_wav_test --model <model.bin> --text "打开地图" --output /tmp/open_map.wav
 *
 * 退出码：
 *   0 成功
 *   2 参数错误
 *   3 模型加载失败
 *   4 合成失败
 *   5 WAV 写入失败
 *   6 WAV 回读验证失败
 */

#include "../tts/include/TTSModel.h"
#include "../audio/wav_io.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

static void printHelp()
{
    std::cout << "Usage: tts_wav_test --model <model.bin> --text <UTF-8 text> --output <output.wav>\n"
              << "\n"
              << "Options:\n"
              << "  --model <path>    Path to single_speaker_fast.bin\n"
              << "  --text <string>   UTF-8 Chinese text to synthesize\n"
              << "  --output <path>   Output WAV file path\n"
              << "  --help            Show this help\n"
              << "\n"
              << "Exit codes:\n"
              << "  0 success\n"
              << "  2 argument error\n"
              << "  3 model load failed\n"
              << "  4 synthesis failed\n"
              << "  5 WAV write failed\n"
              << "  6 WAV readback verification failed\n";
}

static bool fileExists(const std::string &path)
{
    std::ifstream f(path);
    return f.good();
}

int main(int argc, char *argv[])
{
    std::string modelPath;
    std::string text;
    std::string outputPath;

    // 解析参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printHelp();
            return 0;
        } else if (arg == "--model") {
            if (++i >= argc) {
                std::cerr << "ERROR: --model requires a value\n";
                return 2;
            }
            modelPath = argv[i];
        } else if (arg == "--text") {
            if (++i >= argc) {
                std::cerr << "ERROR: --text requires a value\n";
                return 2;
            }
            text = argv[i];
        } else if (arg == "--output") {
            if (++i >= argc) {
                std::cerr << "ERROR: --output requires a value\n";
                return 2;
            }
            outputPath = argv[i];
        } else {
            std::cerr << "ERROR: unknown argument: " << arg << "\n";
            return 2;
        }
    }

    // 校验必填参数
    if (modelPath.empty()) {
        std::cerr << "ERROR: --model is required\n";
        return 2;
    }

    if (text.empty()) {
        std::cerr << "ERROR: --text is required and must not be empty\n";
        return 2;
    }

    // 检查 text 是否全是空白
    bool allWhitespace = true;
    for (char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            allWhitespace = false;
            break;
        }
    }
    if (allWhitespace) {
        std::cerr << "ERROR: --text must not be whitespace only\n";
        return 2;
    }

    if (outputPath.empty()) {
        std::cerr << "ERROR: --output is required\n";
        return 2;
    }

    // 检查模型文件存在
    if (!fileExists(modelPath)) {
        std::cerr << "ERROR: model file not found: " << modelPath << "\n";
        return 3;
    }

    std::cout << "INPUT_TEXT=" << text << "\n";
    std::cout << "MODEL_PATH=" << modelPath << "\n";

    // 创建 TTSModel（构造函数加载失败时抛 std::runtime_error）
    std::unique_ptr<TTSModel> model;
    try {
        model = std::make_unique<TTSModel>(modelPath);
    } catch (const std::runtime_error &e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        std::cout << "TTS_RESULT=FAIL\n";
        std::cout << "ERROR=" << e.what() << "\n";
        return 3;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: TTS model construction failed: " << e.what() << "\n";
        std::cout << "TTS_RESULT=FAIL\n";
        std::cout << "ERROR=TTS construction failed\n";
        return 3;
    }

    if (!model->isLoaded()) {
        std::cerr << "ERROR: model load failed (isLoaded=false after construction)\n";
        std::cout << "TTS_RESULT=FAIL\n";
        std::cout << "ERROR=model load failed\n";
        return 3;
    }

    // 推理
    int32_t audioLen = 0;
    int16_t *pcmData = model->infer(text, audioLen);

    if (pcmData == nullptr || audioLen <= 0) {
        std::cerr << "ERROR: synthesis returned null or empty\n";
        std::cout << "TTS_RESULT=FAIL\n";
        std::cout << "ERROR=synthesis returned null or empty\n";
        return 4;
    }

    // RAII 确保释放 PCM 数据
    struct PcmGuard {
        TTSModel &model;
        int16_t *data;
        ~PcmGuard() { if (data) model.free_data(data); }
    } guard{*model, pcmData};

    const size_t sampleCount = static_cast<size_t>(audioLen);

    std::cout << "WAV_PATH=" << outputPath << "\n";
    std::cout << "WAV_RATE=" << TTSModel::kSampleRate << "\n";
    std::cout << "WAV_CHANNELS=" << TTSModel::kChannels << "\n";
    std::cout << "WAV_BITS=" << TTSModel::kBitsPerSample << "\n";
    std::cout << "WAV_SAMPLES=" << sampleCount << "\n";
    std::cout << "WAV_DURATION_MS=" << (sampleCount * 1000 / TTSModel::kSampleRate) << "\n";

    // 写入 WAV
    std::string writeErr;
    if (!voiceaudio::writePcm16Wave(outputPath, pcmData, sampleCount,
                                    static_cast<uint32_t>(TTSModel::kSampleRate),
                                    static_cast<uint16_t>(TTSModel::kChannels), &writeErr)) {
        std::cerr << "ERROR: WAV write failed: " << writeErr << "\n";
        std::cout << "TTS_RESULT=FAIL\n";
        std::cout << "ERROR=WAV write failed: " << writeErr << "\n";
        return 5;
    }

    // 回读验证
    voiceaudio::Pcm16Wave wave;
    std::string readErr;
    if (!voiceaudio::readPcm16Wave(outputPath, &wave, &readErr)) {
        std::cerr << "ERROR: WAV readback failed: " << readErr << "\n";
        std::cout << "TTS_RESULT=FAIL\n";
        std::cout << "ERROR=WAV readback failed: " << readErr << "\n";
        std::remove(outputPath.c_str());
        return 6;
    }

    if (wave.sampleRate != static_cast<uint32_t>(TTSModel::kSampleRate) ||
        wave.channels != static_cast<uint16_t>(TTSModel::kChannels) ||
        wave.bitsPerSample != static_cast<uint16_t>(TTSModel::kBitsPerSample) ||
        wave.samples.size() != sampleCount) {
        std::cerr << "ERROR: WAV metadata mismatch after readback\n";
        std::cout << "TTS_RESULT=FAIL\n";
        std::cout << "ERROR=WAV metadata mismatch\n";
        std::remove(outputPath.c_str());
        return 6;
    }

    std::cout << "TTS_RESULT=PASS\n";
    return 0;
}
