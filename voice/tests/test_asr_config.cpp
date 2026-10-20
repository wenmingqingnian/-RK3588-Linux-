/**
 * @file test_asr_config.cpp
 * @brief AsrEngine::loadConfig() 配置解析单元测试（TDD RED）。
 *
 * 覆盖：
 *   - 测试1：缺少新配置项时使用默认值
 *     decodingMethod=="greedy_search", hotwordsFile.empty(), hotwordsScore==1.5f
 *   - 测试2：正确解析有效配置
 *     asr_decoding_method=modified_beam_search / asr_hotwords_file / asr_hotwords_score=2.0
 *   - 测试3：非法解码方式回退默认值
 *     asr_decoding_method=unknown_decoder => "greedy_search"
 *   - 测试4：非法分数回退默认值
 *     asr_hotwords_score=abc / -1 / 0 => 1.5f
 *
 * 测试直接调用 voice::AsrEngine::loadConfig()。
 * 测试删除自身创建的临时配置文件。
 * 浮点比较使用容差。
 *
 * RED 阶段：因 AsrConfig 尚无 decodingMethod/hotwordsFile/hotwordsScore 字段，
 * 本测试编译失败 —— 这是预期的 RED 状态，证明功能尚未实现。
 */

#include "../asr/AsrEngine.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

int g_passCount = 0;
int g_failCount = 0;

#define CHECK(cond, msg) do { \
    if ((cond)) { \
        ++g_passCount; \
    } else { \
        ++g_failCount; \
        std::cerr << "FAIL: " << (msg) << std::endl; \
    } \
} while(0)

#define CHECK_EQ_STR(actual, expected, msg) do { \
    if ((actual) == (expected)) { \
        ++g_passCount; \
    } else { \
        ++g_failCount; \
        std::cerr << "FAIL: " << (msg) \
                  << " (got=\"" << (actual) << "\", expected=\"" << (expected) << "\")" \
                  << std::endl; \
    } \
} while(0)

#define CHECK_EQ_FLOAT(actual, expected, tol, msg) do { \
    if (std::fabs((actual) - (expected)) <= (tol)) { \
        ++g_passCount; \
    } else { \
        ++g_failCount; \
        std::cerr << "FAIL: " << (msg) \
                  << " (got=" << (actual) << ", expected=" << (expected) \
                  << ", tol=" << (tol) << ")" << std::endl; \
    } \
} while(0)

// 浮点比较容差：足够小以识别明显错误，又允许 stof 精度损失。
constexpr float kFloatTol = 1e-3f;

// 默认期望值（必须与生产代码默认值一致）。
constexpr const char *kDefaultDecoding = "greedy_search";
constexpr float kDefaultHotwordsScore = 1.5f;

/**
 * @brief 写入临时配置文件，返回路径。
 * 调用方负责删除该文件。
 *
 * 使用 mkstemp 原子创建仅当前用户可访问的临时文件，避免先生成路径、
 * 后创建文件所产生的竞态。suffix 只用于区分各测试用例。
 */
std::string writeTempConfig(const std::string &content, const char *suffix)
{
    const std::string safeSuffix = suffix ? suffix : "config";
    const std::string pathTemplate =
        "/tmp/asr_config_test_" + safeSuffix + "_XXXXXX";
    std::vector<char> pathBuffer(pathTemplate.begin(), pathTemplate.end());
    pathBuffer.push_back('\0');

    const int fd = ::mkstemp(pathBuffer.data());
    if (fd < 0) {
        std::cerr << "ERROR: cannot create temp config: "
                  << std::strerror(errno) << std::endl;
        return {};
    }

    const std::string path(pathBuffer.data());
    const char *data = content.data();
    std::size_t remaining = content.size();

    while (remaining > 0) {
        const ssize_t written = ::write(fd, data, remaining);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            const int savedErrno = written < 0 ? errno : EIO;
            ::close(fd);
            std::remove(path.c_str());
            std::cerr << "ERROR: cannot write temp config " << path << ": "
                      << std::strerror(savedErrno) << std::endl;
            return {};
        }

        data += written;
        remaining -= static_cast<std::size_t>(written);
    }

    if (::close(fd) != 0) {
        const int savedErrno = errno;
        std::remove(path.c_str());
        std::cerr << "ERROR: cannot close temp config " << path << ": "
                  << std::strerror(savedErrno) << std::endl;
        return {};
    }

    return path;
}

void removeFile(const std::string &path)
{
    if (!path.empty()) {
        std::remove(path.c_str());
    }
}

// --- 测试1：缺少新配置项时使用默认值 ---
// 配置仅含 enabled，不含 asr_decoding_method / asr_hotwords_file / asr_hotwords_score。
// 期望：
//   decodingMethod == "greedy_search"
//   hotwordsFile.empty() == true
//   hotwordsScore == 1.5f
void testDefaultsWhenMissing()
{
    const std::string content =
        "asr_enabled=true\n";
    std::string path = writeTempConfig(content, "defaults");
    CHECK(!path.empty(), "test1: temp config creation failed");

    voice::AsrConfig cfg = voice::AsrEngine::loadConfig(path);
    removeFile(path);

    CHECK(cfg.enabled == true, "test1: enabled should be true");
    CHECK_EQ_STR(cfg.decodingMethod, std::string(kDefaultDecoding),
                 "test1: decodingMethod default");
    CHECK(cfg.hotwordsFile.empty(), "test1: hotwordsFile should be empty by default");
    CHECK_EQ_FLOAT(cfg.hotwordsScore, kDefaultHotwordsScore, kFloatTol,
                   "test1: hotwordsScore default");
}

// --- 测试2：正确解析有效配置 ---
// 配置：
//   asr_enabled=true
//   asr_decoding_method=modified_beam_search
//   asr_hotwords_file=./config/asr_hotwords.txt
//   asr_hotwords_score=2.0
// 期望：
//   enabled == true
//   decodingMethod == "modified_beam_search"
//   hotwordsFile == "./config/asr_hotwords.txt"
//   hotwordsScore 接近 2.0
void testValidConfig()
{
    const std::string content =
        "asr_enabled=true\n"
        "asr_decoding_method=modified_beam_search\n"
        "asr_hotwords_file=./config/asr_hotwords.txt\n"
        "asr_hotwords_score=2.0\n";
    std::string path = writeTempConfig(content, "valid");
    CHECK(!path.empty(), "test2: temp config creation failed");

    voice::AsrConfig cfg = voice::AsrEngine::loadConfig(path);
    removeFile(path);

    CHECK(cfg.enabled == true, "test2: enabled should be true");
    CHECK_EQ_STR(cfg.decodingMethod, std::string("modified_beam_search"),
                 "test2: decodingMethod");
    CHECK_EQ_STR(cfg.hotwordsFile, std::string("./config/asr_hotwords.txt"),
                 "test2: hotwordsFile");
    CHECK_EQ_FLOAT(cfg.hotwordsScore, 2.0f, kFloatTol,
                   "test2: hotwordsScore");
}

// --- 测试3：非法解码方式回退默认值 ---
// 配置：asr_decoding_method=unknown_decoder
// 期望：decodingMethod == "greedy_search"
void testInvalidDecodingFallback()
{
    const std::string content =
        "asr_enabled=true\n"
        "asr_decoding_method=unknown_decoder\n";
    std::string path = writeTempConfig(content, "bad_decoder");
    CHECK(!path.empty(), "test3: temp config creation failed");

    voice::AsrConfig cfg = voice::AsrEngine::loadConfig(path);
    removeFile(path);

    CHECK_EQ_STR(cfg.decodingMethod, std::string(kDefaultDecoding),
                 "test3: invalid decoding method should fall back to greedy_search");
}

// --- 测试4：非法分数回退默认值 ---
// 分别验证：
//   asr_hotwords_score=abc   (非数字)
//   asr_hotwords_score=-1    (负数)
//   asr_hotwords_score=0     (非正数)
// 期望：hotwordsScore == 1.5f
void testInvalidScoreFallback()
{
    const char *kBadScores[] = { "abc", "-1", "0" };
    for (const char *bad : kBadScores) {
        std::string content =
            "asr_enabled=true\n"
            "asr_hotwords_score=" + std::string(bad) + "\n";
        std::string path = writeTempConfig(content, "bad_score");
        CHECK(!path.empty(), "test4: temp config creation failed");

        voice::AsrConfig cfg = voice::AsrEngine::loadConfig(path);
        removeFile(path);

        CHECK_EQ_FLOAT(cfg.hotwordsScore, kDefaultHotwordsScore, kFloatTol,
                       std::string("test4: invalid score '") + bad +
                       "' should fall back to 1.5");
    }
}

} // namespace

int main()
{
    testDefaultsWhenMissing();      // 1
    testValidConfig();              // 2
    testInvalidDecodingFallback();  // 3
    testInvalidScoreFallback();     // 4

    std::cout << "asr_config tests: " << g_passCount << " passed, "
              << g_failCount << " failed" << std::endl;

    return g_failCount > 0 ? 1 : 0;
}
