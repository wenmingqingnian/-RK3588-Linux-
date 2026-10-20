/**
 * @file asr_wav_test.cpp
 * @brief WAV→ASR→指令 命令行工具。
 *
 * 读取 WAV 文件，通过 sherpa-onnx 离线 ASR 识别中文，
 * 调用 vehiclecommand::parseCabinCommand() 解析为车载指令。
 *
 * 不打开 ALSA，不启动 Qt GUI，不启动 LLM/TTS 服务进程，不访问 NPU。
 *
 * 用法：
 *   asr_wav_test --config <asr_settings_offline.conf> --wav /tmp/open_map.wav \
 *                --expect-command MapShow
 *
 * 退出码：
 *   0 成功
 *   2 参数错误
 *   3 WAV 读取或格式错误
 *   4 ASR 配置或模型初始化失败
 *   5 未获得识别文字
 *   6 预期文字不匹配
 *   7 预期指令不匹配
 *   8 ASR ABI 或运行错误
 */

#include "../asr/AsrEngine.h"
#include "../audio/wav_io.h"
#include "../../common/vehiclecommand.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// --- 音频格式与 ASR 喂入参数（sherpa-onnx streaming 模型要求 16kHz） ---
// 这些是技术常数而非环境参数，用具名常量集中管理，避免魔法数字。
constexpr int32_t kRequiredSampleRate = 16000;   // sherpa-onnx streaming 模型固有声学采样率
constexpr int16_t kRequiredChannels = 1;         // 单声道
constexpr int16_t kRequiredBitsPerSample = 16;   // PCM 16-bit
constexpr int32_t kAsrChunkSamples = 1600;       // 100ms @ 16kHz，单次喂入粒度
constexpr int32_t kAsrSilenceSamples = 16000;    // 1s @ 16kHz，inputFinished 不可用时的静音 fallback
constexpr int32_t kAsrMaxDecodeCalls = 1000;     // 解码循环上限，防止无限循环

// --- 参数解析结果 ---
struct AsrTestArgs {
    std::string configPath;
    std::string wavPath;
    std::string expectText;
    std::string expectCommand;
    bool hasExpectText = false;
    bool hasExpectCommand = false;
};

static void printHelp()
{
    std::cout << "Usage: asr_wav_test --config <conf> --wav <input.wav>\n"
              << "                     [--expect-text <text>] [--expect-command <Cmd>]\n"
              << "\n"
              << "Options:\n"
              << "  --config <path>       Path to asr_settings_offline.conf\n"
              << "  --wav <path>          Input WAV file (16kHz/mono/PCM16)\n"
              << "  --expect-text <str>   Expected ASR text (optional)\n"
              << "  --expect-command <s>  Expected CabinCommand name (optional)\n"
              << "  --help                Show this help\n"
              << "\n"
              << "Exit codes:\n"
              << "  0 success\n"
              << "  2 argument error\n"
              << "  3 WAV read/format error\n"
              << "  4 ASR config/init error\n"
              << "  5 no ASR text obtained\n"
              << "  6 expected text mismatch\n"
              << "  7 expected command mismatch\n"
              << "  8 ASR ABI/runtime error\n";
}

/**
 * @brief 解析命令行参数。参数错误时打印用法并 exit(2)。
 */
static AsrTestArgs parseArgs(int argc, char *argv[])
{
    AsrTestArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printHelp();
            std::exit(0);
        } else if (arg == "--config") {
            if (++i >= argc) { std::cerr << "ERROR: --config requires a value\n"; std::exit(2); }
            args.configPath = argv[i];
        } else if (arg == "--wav") {
            if (++i >= argc) { std::cerr << "ERROR: --wav requires a value\n"; std::exit(2); }
            args.wavPath = argv[i];
        } else if (arg == "--expect-text") {
            if (++i >= argc) { std::cerr << "ERROR: --expect-text requires a value\n"; std::exit(2); }
            args.expectText = argv[i];
            args.hasExpectText = true;
        } else if (arg == "--expect-command") {
            if (++i >= argc) { std::cerr << "ERROR: --expect-command requires a value\n"; std::exit(2); }
            args.expectCommand = argv[i];
            args.hasExpectCommand = true;
        } else {
            std::cerr << "ERROR: unknown argument: " << arg << "\n";
            std::exit(2);
        }
    }

    if (args.configPath.empty()) {
        std::cerr << "ERROR: --config is required\n";
        std::exit(2);
    }
    if (args.wavPath.empty()) {
        std::cerr << "ERROR: --wav is required\n";
        std::exit(2);
    }
    return args;
}

/**
 * @brief 读取 WAV 并严格校验格式（16kHz/mono/PCM16）。输出元数据与 PCM 诊断。
 * @return 0 成功；3 表示读取或格式错误（已打印 ASR_RESULT=FAIL）。
 */
static int loadAndReportWav(const std::string &path, voiceaudio::Pcm16Wave *wave)
{
    std::string wavErr;
    if (!voiceaudio::readPcm16Wave(path, wave, &wavErr)) {
        std::cerr << "ERROR: WAV read failed: " << wavErr << "\n";
        std::cout << "ASR_RESULT=FAIL\n";
        std::cout << "ERROR=WAV read failed: " << wavErr << "\n";
        return 3;
    }

    if (wave->sampleRate != static_cast<uint32_t>(kRequiredSampleRate)) {
        std::cerr << "ERROR: sample rate must be " << kRequiredSampleRate
                  << ", got " << wave->sampleRate << "\n";
        std::cout << "ASR_RESULT=FAIL\n";
        std::cout << "ERROR=sample rate mismatch\n";
        return 3;
    }
    if (wave->channels != static_cast<uint16_t>(kRequiredChannels)) {
        std::cerr << "ERROR: channels must be " << kRequiredChannels
                  << ", got " << wave->channels << "\n";
        std::cout << "ASR_RESULT=FAIL\n";
        std::cout << "ERROR=channels mismatch\n";
        return 3;
    }
    if (wave->bitsPerSample != static_cast<uint16_t>(kRequiredBitsPerSample)) {
        std::cerr << "ERROR: bits per sample must be " << kRequiredBitsPerSample
                  << ", got " << wave->bitsPerSample << "\n";
        std::cout << "ASR_RESULT=FAIL\n";
        std::cout << "ERROR=bits mismatch\n";
        return 3;
    }

    std::cout << "WAV_PATH=" << path << "\n";
    std::cout << "WAV_RATE=" << wave->sampleRate << "\n";
    std::cout << "WAV_CHANNELS=" << wave->channels << "\n";
    std::cout << "WAV_BITS=" << wave->bitsPerSample << "\n";
    std::cout << "WAV_SAMPLES=" << wave->samples.size() << "\n";

    // PCM 诊断：peak 与 RMS，用于排查"全零静音"或"削波"
    int16_t peak = 0;
    double sumSq = 0.0;
    for (int16_t s : wave->samples) {
        int16_t absVal = static_cast<int16_t>(std::abs(static_cast<int>(s)));
        if (absVal > peak) peak = absVal;
        sumSq += static_cast<double>(s) * s;
    }
    double rms = std::sqrt(sumSq / wave->samples.size());
    std::cout << "PCM_PEAK=" << peak << "\n";
    std::cout << "PCM_RMS=" << static_cast<int>(rms) << "\n";

    return 0;
}

/**
 * @brief 初始化 ASR 引擎并报告 API 可用性、版本与 ABI 兼容性。
 * @return 0 成功；4 表示配置或初始化失败。
 */
static int initAsrEngine(const std::string &configPath, voice::AsrEngine *asr)
{
    voice::AsrConfig config = voice::AsrEngine::loadConfig(configPath);
    if (!config.enabled) {
        std::cerr << "ERROR: ASR disabled in config\n";
        std::cout << "ASR_RESULT=FAIL\n";
        std::cout << "ERROR=ASR disabled in config\n";
        return 4;
    }

    if (!asr->init(config)) {
        std::cerr << "ERROR: ASR engine init failed\n";
        std::cout << "ASR_RESULT=FAIL\n";
        std::cout << "ERROR=ASR init failed\n";
        return 4;
    }

    // 报告运行时 sherpa-onnx 版本与 Git SHA/Date（来自 dlsym 加载的库）
    std::cout << "SHERPA_VERSION=" << asr->runtimeVersion() << "\n";
    std::cout << "SHERPA_GIT_SHA=" << asr->runtimeGitSha() << "\n";
    std::cout << "SHERPA_GIT_DATE=" << asr->runtimeGitDate() << "\n";

    // 仅当版本与 Git SHA 同时匹配 1.13.3 / 330609da 时为 1，
    // 此时 getResult() 按官方结构读取 result->text。
    std::cout << "RESULT_ABI_COMPATIBLE=" << (asr->isResultAbiCompatible() ? 1 : 0) << "\n";

    // InputFinished 是可选 API；板端 1.13.3 已导出，预期为 1
    std::cout << "INPUT_FINISHED_API=" << (asr->hasInputFinishedApi() ? 1 : 0) << "\n";

    // 结果读取路径：经版本校验的官方 1.13.3 类型化访问
    std::cout << "RESULT_API=typed-abi-1.13.3\n";

    if (!asr->isResultAbiCompatible()) {
        std::cerr << "ERROR: sherpa result ABI not compatible; "
                     "expected version=1.13.3 sha=330609da, "
                     "actual version=" << asr->runtimeVersion()
                  << " sha=" << asr->runtimeGitSha() << "\n";
        std::cout << "ASR_RESULT=FAIL\n";
        std::cout << "ERROR=result ABI not compatible\n";
        return 8; // ASR ABI 或运行错误
    }
    return 0;
}

/**
 * @brief 执行 ASR 识别：int16→float 转换、分块喂入、解码、文件结束处理。
 */
static void runRecognition(voice::AsrEngine &asr, const voiceaudio::Pcm16Wave &wave,
                           std::string *asrText, int *decodeCalls, bool *endpointDetected)
{
    // int16 → float[-1,1] 转换
    std::vector<float> floatSamples(wave.samples.size());
    for (size_t i = 0; i < wave.samples.size(); ++i) {
        floatSamples[i] = static_cast<float>(wave.samples[i]) / 32768.0f;
    }

    // 分块喂入并解码
    size_t offset = 0;
    while (offset < floatSamples.size()) {
        size_t remaining = floatSamples.size() - offset;
        int32_t toFeed = static_cast<int32_t>(
            remaining < static_cast<size_t>(kAsrChunkSamples)
                ? remaining : static_cast<size_t>(kAsrChunkSamples));

        asr.acceptWaveform(floatSamples.data() + offset, toFeed);
        offset += toFeed;

        while (asr.isReadyToDecode() && *decodeCalls < kAsrMaxDecodeCalls) {
            asr.decode();
            ++(*decodeCalls);
        }
    }

    // 文件结束处理：优先 inputFinished API，不可用则追加 1 秒静音触发 endpoint
    if (asr.hasInputFinishedApi()) {
        asr.inputFinished();
        while (asr.isReadyToDecode() && *decodeCalls < kAsrMaxDecodeCalls) {
            asr.decode();
            ++(*decodeCalls);
        }
    } else {
        std::vector<float> silence(kAsrSilenceSamples, 0.0f);
        asr.acceptWaveform(silence.data(), kAsrSilenceSamples);
        while (asr.isReadyToDecode() && *decodeCalls < kAsrMaxDecodeCalls) {
            asr.decode();
            ++(*decodeCalls);
        }
    }

    *endpointDetected = asr.isEndpoint();
    *asrText = asr.getResult();
}

/**
 * @brief 检查识别结果是否符合预期。
 * @return 0 成功；5 无文字；6 文字不匹配；7 指令不匹配。
 */
static int checkResult(const std::string &asrText, const AsrTestArgs &args,
                       vehiclecommand::CabinCommand cmd)
{
    if (asrText.empty()) {
        std::cout << "ASR_RESULT=FAIL\n";
        std::cout << "ERROR=no non-empty ASR result\n";
        return 5;
    }

    if (args.hasExpectText && asrText != args.expectText) {
        std::cout << "ASR_RESULT=FAIL\n";
        std::cout << "ERROR=expected text '" << args.expectText
                  << "' but got '" << asrText << "'\n";
        return 6;
    }

    if (args.hasExpectCommand) {
        const char *cmdStr = vehiclecommand::toString(cmd);
        if (cmdStr != args.expectCommand) {
            std::cout << "ASR_RESULT=FAIL\n";
            std::cout << "ERROR=expected command '" << args.expectCommand
                      << "' but got '" << cmdStr << "'\n";
            return 7;
        }
    }

    std::cout << "ASR_RESULT=PASS\n";
    return 0;
}

int main(int argc, char *argv[])
{
    AsrTestArgs args = parseArgs(argc, argv);

    // 1. 读取并校验 WAV
    voiceaudio::Pcm16Wave wave;
    if (int rc = loadAndReportWav(args.wavPath, &wave)) {
        return rc;
    }

    // 2. 初始化 ASR 引擎
    voice::AsrEngine asr;
    if (int rc = initAsrEngine(args.configPath, &asr)) {
        return rc;
    }

    // 3. 执行识别
    std::string asrText;
    int decodeCalls = 0;
    bool endpointDetected = false;
    runRecognition(asr, wave, &asrText, &decodeCalls, &endpointDetected);

    std::cout << "DECODE_CALLS=" << decodeCalls << "\n";
    std::cout << "ENDPOINT_DETECTED=" << (endpointDetected ? 1 : 0) << "\n";

    // 4. 释放 ASR 资源（析构也会调 deinit，这里显式释放便于日志时序清晰）
    asr.deinit();

    // 5. 输出识别文本与解析指令
    std::cout << "ASR_TEXT=" << asrText << "\n";
    vehiclecommand::CabinCommand cmd = vehiclecommand::parseCabinCommand(asrText);
    std::cout << "COMMAND=" << vehiclecommand::toString(cmd) << "\n";
    if (args.hasExpectCommand) {
        std::cout << "EXPECTED_COMMAND=" << args.expectCommand << "\n";
    }

    // 6. 检查结果
    return checkResult(asrText, args, cmd);
}
