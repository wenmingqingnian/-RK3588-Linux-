/**
 * @file test_asr_abi.cpp
 * @brief sherpa_onnx_abi_1_13_3.h 单元测试（纯 C++17，无外部依赖）。
 *
 * 覆盖 isCompatible() 各种取值与 ABI 结构布局不变量。
 * 本测试不加载真实动态库、不依赖板端模型，可在 Windows/Linux/构建机直接运行。
 */

#include "../asr/sherpa_onnx_abi_1_13_3.h"

#include <cstddef>
#include <cstdint>
#include <iostream>

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

// 编译期结构布局不变量（任一失败即编译错误）
static_assert(
    std::is_standard_layout<sherpa_abi_1_13_3::OnlineRecognizerResult>::value,
    "OnlineRecognizerResult must be standard layout");

static_assert(
    offsetof(sherpa_abi_1_13_3::OnlineRecognizerResult, text) == 0,
    "text field must be at offset 0 (sherpa 1.13.3 ABI)");

static_assert(
    offsetof(sherpa_abi_1_13_3::OnlineRecognizerResult, tokens) >
    offsetof(sherpa_abi_1_13_3::OnlineRecognizerResult, text),
    "tokens must follow text");

static_assert(
    offsetof(sherpa_abi_1_13_3::OnlineRecognizerResult, json) >
    offsetof(sherpa_abi_1_13_3::OnlineRecognizerResult, count),
    "json must follow count");

static_assert(
    sizeof(sherpa_abi_1_13_3::OnlineRecognizerResult) >=
    sizeof(const char *) * 4 + sizeof(float *) + sizeof(int32_t),
    "OnlineRecognizerResult size must be at least 4 ptr + 1 float* + 1 int32_t "
    "(padding may increase actual size on 64-bit platforms)");

static void testCompatible()
{
    using sherpa_abi_1_13_3::isCompatible;
    CHECK(isCompatible("1.13.3", "330609da"),
          "test 1: version=1.13.3 sha=330609da must be compatible");
}

static void testWrongSha()
{
    using sherpa_abi_1_13_3::isCompatible;
    CHECK(!isCompatible("1.13.3", "deadbeef"),
          "test 2: version=1.13.3 sha=deadbeef must NOT be compatible");
}

static void testWrongVersion()
{
    using sherpa_abi_1_13_3::isCompatible;
    CHECK(!isCompatible("1.13.2", "330609da"),
          "test 3: version=1.13.2 sha=330609da must NOT be compatible");
}

static void testNullVersion()
{
    using sherpa_abi_1_13_3::isCompatible;
    CHECK(!isCompatible(nullptr, "330609da"),
          "test 4: version=nullptr must NOT be compatible");
}

static void testNullSha()
{
    using sherpa_abi_1_13_3::isCompatible;
    CHECK(!isCompatible("1.13.3", nullptr),
          "test 5: sha=nullptr must NOT be compatible");
}

static void testEmptyStrings()
{
    using sherpa_abi_1_13_3::isCompatible;
    CHECK(!isCompatible("", ""),
          "test 6: empty strings must NOT be compatible");
    CHECK(!isCompatible("", "330609da"),
          "test 6b: empty version must NOT be compatible");
    CHECK(!isCompatible("1.13.3", ""),
          "test 6c: empty sha must NOT be compatible");
}

static void testBothNull()
{
    using sherpa_abi_1_13_3::isCompatible;
    CHECK(!isCompatible(nullptr, nullptr),
          "test 7: both nullptr must NOT be compatible");
}

static void testPrefixMatch()
{
    using sherpa_abi_1_13_3::isCompatible;
    // 1.13.3.1 不是 1.13.3，必须不匹配
    CHECK(!isCompatible("1.13.3.1", "330609da"),
          "test 8: prefix version match must NOT be compatible");
    // sha 前缀同理
    CHECK(!isCompatible("1.13.3", "330609d"),
          "test 9: prefix sha match must NOT be compatible");
    CHECK(!isCompatible("1.13.3", "330609da00"),
          "test 10: longer sha must NOT be compatible");
}

int main()
{
    testCompatible();      // 1
    testWrongSha();        // 2
    testWrongVersion();    // 3
    testNullVersion();     // 4
    testNullSha();         // 5
    testEmptyStrings();    // 6, 6b, 6c
    testBothNull();        // 7
    testPrefixMatch();     // 8, 9, 10

    std::cout << "asr_abi tests: " << g_passCount << " passed, "
              << g_failCount << " failed" << std::endl;

    return g_failCount > 0 ? 1 : 0;
}
