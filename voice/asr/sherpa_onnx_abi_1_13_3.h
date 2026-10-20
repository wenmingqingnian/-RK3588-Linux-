#ifndef SHERPA_ONNX_ABI_1_13_3_H
#define SHERPA_ONNX_ABI_1_13_3_H

/**
 * @file sherpa_onnx_abi_1_13_3.h
 * @brief sherpa-onnx 1.13.3 在线识别 / 关键词检测 结果结构 ABI 定义（最小子集）。
 *
 * 仅保存本任务读取识别结果所需要的最小官方结构定义，不复制整个 c-api.h。
 *
 * 板端已确认事实：
 *   - 库路径：/usr/lib/aarch64-linux-gnu/libsherpa-onnx-c-api.so
 *   - 库 SHA256：c713c6c6ca7fcaff01607768d3a163f73aae0c83e476bc1bcbce7839b9695507
 *   - 运行时版本：1.13.3
 *   - Git SHA：330609da
 *   - Git Date：Mon Jun 15 07:50:54 2026
 *
 * 官方 1.13.3 头文件来源（仅作为 ABI 验证参考，不写入 CMake，不直接 include）：
 *   /home/quark/.local/lib/python3.9/site-packages/sherpa_onnx/include/sherpa-onnx/c-api/c-api.h
 *
 * 板端动态库导出：
 *   SherpaOnnxGetOnlineStreamResult
 *   SherpaOnnxDestroyOnlineRecognizerResult
 *   SherpaOnnxOnlineStreamInputFinished
 *   SherpaOnnxGetVersionStr / SherpaOnnxGetGitSha1 / SherpaOnnxGetGitDate
 *   SherpaOnnxCreateKeywordSpotter / SherpaOnnxDestroyKeywordSpotter
 *   SherpaOnnxCreateKeywordStream / SherpaOnnxCreateKeywordStreamWithKeywords
 *   SherpaOnnxIsKeywordStreamReady
 *   SherpaOnnxDecodeKeywordStream
 *   SherpaOnnxGetKeywordResult / SherpaOnnxDestroyKeywordResult
 *   SherpaOnnxResetKeywordStream
 *
 * 板端动态库不导出：
 *   SherpaOnnxOnlineStreamGetResultText
 *
 * 因此读取识别结果必须按官方 1.13.3 的 SherpaOnnxOnlineRecognizerResult 结构布局
 * 直接访问 result->text，并先用 SherpaOnnxGetVersionStr / SherpaOnnxGetGitSha1
 * 验证运行时库恰好是已确认的版本，禁止继续使用未经验证的偏移 fallback。
 *
 * KeywordSpotter（KWS）结果结构 SherpaOnnxKeywordResult 同样按 1.13.3 官方布局
 * 直接访问 result->keyword；KWS 自 sherpa-onnx 1.10.x 起就已存在于 C API，
 * 1.13.3 是稳定 API，无需单独校验 KWS 版本，沿用 isCompatible 即可。
 */

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace sherpa_abi_1_13_3 {

/**
 * @brief sherpa-onnx 1.13.3 在线识别结果结构（官方字段与顺序）。
 *
 * 仅在本任务读取 text 字段时使用，不参与配置构造。
 */
struct OnlineRecognizerResult {
    const char *text;
    const char *tokens;
    const char *const *tokens_arr;
    float *timestamps;
    int32_t count;
    const char *json;
};

static_assert(
    std::is_standard_layout<OnlineRecognizerResult>::value,
    "sherpa result ABI type must be standard layout");

static_assert(
    offsetof(OnlineRecognizerResult, text) == 0,
    "sherpa 1.13.3 text field ABI mismatch: text must be at offset 0");

/**
 * @brief sherpa-onnx 1.13.3 关键词检测结果结构（官方字段与顺序）。
 *
 * 与 OnlineRecognizerResult 同样按官方 c-api.h 1.13.3 布局直接访问，
 * 仅用 keyword 字段（命中关键词的显示名，如 "你好小问"）。
 *
 * 注意：SherpaOnnxGetKeywordResult 返回的指针必须由
 *       SherpaOnnxDestroyKeywordResult 显式释放（与 OnlineRecognizerResult
 *       由 SherpaOnnxDestroyOnlineRecognizerResult 释放对称）。
 */
struct KeywordResult {
    const char *keyword;            // 命中关键词显示名（如 "你好小问"）
    const char *const *tokens;      // token 数组（拼音/汉字/BPE）
    int32_t count;                  // tokens 数量
    const float *timestamps;        // 每个 token 的时间戳（秒）
    float start_time;               // 当前段起始时间
    const char *json;               // JSON 序列化结果
};

static_assert(
    std::is_standard_layout<KeywordResult>::value,
    "sherpa kws result ABI type must be standard layout");

static_assert(
    offsetof(KeywordResult, keyword) == 0,
    "sherpa 1.13.3 keyword field ABI mismatch: keyword must be at offset 0");

/**
 * @brief 判断运行时库是否与已确认的 1.13.3 / SHA 330609da 兼容。
 *
 * @param version SherpaOnnxGetVersionStr 返回的字符串（库内部裸指针，可为 nullptr）
 * @param gitSha   SherpaOnnxGetGitSha1  返回的字符串（库内部裸指针，可为 nullptr）
 * @return true 仅当版本和 Git SHA 同时匹配
 *
 * 使用 strcmp 实现安全字符串比较，避免引入 std::string 依赖；
 * nullptr 入参直接返回 false。
 */
inline bool isCompatible(const char *version, const char *gitSha)
{
    if (!version || !gitSha) {
        return false;
    }
    const char *kExpectedVersion = "1.13.3";
    const char *kExpectedGitSha = "330609da";
    for (const char *a = version, *b = kExpectedVersion;
         *a == *b; ++a, ++b) {
        if (*a == '\0') {
            // version 匹配，继续检查 gitSha
            for (const char *c = gitSha, *d = kExpectedGitSha;
                 *c == *d; ++c, ++d) {
                if (*c == '\0') {
                    return true;
                }
            }
            return false;
        }
    }
    return false;
}

} // namespace sherpa_abi_1_13_3

#endif // SHERPA_ONNX_ABI_1_13_3_H
