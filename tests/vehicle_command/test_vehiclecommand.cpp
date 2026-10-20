/**
 * @file test_vehiclecommand.cpp
 * @brief vehiclecommand::parseCabinCommand 单元测试。
 *
 * 覆盖：
 *   - 负向：孤立 "播放" 是 ASR 可能产生的截断文本，必须返回 Unknown，
 *           避免把 "播放列表" 等非指令误触发为 MusicPlay
 *   - 正向："打开"+"摄象" => MonitorShow
 *   - 负向："播放器" / "播放列表" / "摄象" / "打开茬" 不得误触发
 *   - 既有规则回归：播放音乐 / 打开地图 / 查看地图 / 打开摄像头 / 查看摄像头 / 下一首 / 查看天气
 *   - 英文指令：next / pause / play music
 *   - 冲突避免：暂停播放 => MusicPause（不是 MusicPlay）；打开音乐 => MusicShow（不是 MusicPlay）
 *
 * 不依赖外部库，纯 C++17，可在 Windows/Linux/构建机直接运行。
 */

#include "../../common/vehiclecommand.h"

#include <iostream>
#include <string>

static int g_passCount = 0;
static int g_failCount = 0;

#define CHECK_EQ(actual, expected, msg) do { \
    if ((actual) == (expected)) { \
        ++g_passCount; \
    } else { \
        ++g_failCount; \
        std::cerr << "FAIL: " << (msg) \
                  << " (got=" << vehiclecommand::toString(actual) \
                  << ", expected=" << vehiclecommand::toString(expected) \
                  << ")" << std::endl; \
    } \
} while(0)

// =========================================================================
// 负向用例（歧义文本拒绝执行）
// =========================================================================

// 孤立 "播放" => Unknown
// "播放" 是 ASR 可能产生的截断文本（如 "播放列表" 被截断为 "播放"），
// 必须返回 Unknown，避免把非指令误触发为 MusicPlay。
static void testPlayAloneNoMatch()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u64ad\u653e"), CabinCommand::Unknown,
             "test 1: exact \"\u64ad\u653e\" must be Unknown because an incomplete ASR result is ambiguous");
}

// =========================================================================
// 正向用例（容错规则）
// =========================================================================

// "打开摄象" => MonitorShow（ASR 将 "摄像头" 识别为 "摄象"）
static void testOpenXiangMonitor()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u6253\u5f00\u6444\u8c61"), CabinCommand::MonitorShow,
             "test 2: \"\u6253\u5f00\u6444\u8c61\" must be MonitorShow");
}

// =========================================================================
// 负向用例（不得误触发）
// =========================================================================

// "播放器" => Unknown
static void testPlayerNoMatch()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u64ad\u653e\u5668"), CabinCommand::Unknown,
             "test 3: \"\u64ad\u653e\u5668\" must NOT be MusicPlay");
}

// "播放列表" => Unknown
static void testPlayListNoMatch()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u64ad\u653e\u5217\u8868"), CabinCommand::Unknown,
             "test 4: \"\u64ad\u653e\u5217\u8868\" must NOT be MusicPlay");
}

// "摄象" => Unknown（缺少 "打开"，不得触发 MonitorShow）
static void testXiangAloneNoMatch()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u6444\u8c61"), CabinCommand::Unknown,
             "test 5: \"\u6444\u8c61\" alone must NOT be MonitorShow");
}

// "打开茬" => Unknown（歧义文本，禁止映射为 MapShow）
static void testOpenChaosNoMatch()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u6253\u5f00\u832c"), CabinCommand::Unknown,
             "test 6: \"\u6253\u5f00\u832c\" must NOT be MapShow");
}

// =========================================================================
// 既有规则回归
// =========================================================================

// "播放音乐" => MusicPlay
static void testPlayMusic()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u64ad\u653e\u97f3\u4e50"), CabinCommand::MusicPlay,
             "test 7: \"\u64ad\u653e\u97f3\u4e50\" must be MusicPlay");
}

// "打开地图" => MapShow
static void testOpenMap()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u6253\u5f00\u5730\u56fe"), CabinCommand::MapShow,
             "test 8: \"\u6253\u5f00\u5730\u56fe\" must be MapShow");
}

// "查看地图" => MapShow
static void testViewMap()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u67e5\u770b\u5730\u56fe"), CabinCommand::MapShow,
             "test 9: \"\u67e5\u770b\u5730\u56fe\" must be MapShow");
}

// "打开摄像头" => MonitorShow
static void testOpenCamera()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u6253\u5f00\u6444\u50cf\u5934"), CabinCommand::MonitorShow,
             "test 10: \"\u6253\u5f00\u6444\u50cf\u5934\" must be MonitorShow");
}

// "查看摄像头" => MonitorShow
static void testViewCamera()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u67e5\u770b\u6444\u50cf\u5934"), CabinCommand::MonitorShow,
             "test 11: \"\u67e5\u770b\u6444\u50cf\u5934\" must be MonitorShow");
}

// "下一首" => MusicNext
static void testNextSong()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u4e0b\u4e00\u9996"), CabinCommand::MusicNext,
             "test 12: \"\u4e0b\u4e00\u9996\" must be MusicNext");
}

// "查看天气" => WeatherShow
static void testViewWeather()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u67e5\u770b\u5929\u6c14"), CabinCommand::WeatherShow,
             "test 13: \"\u67e5\u770b\u5929\u6c14\" must be WeatherShow");
}

// =========================================================================
// 英文指令
// =========================================================================

// "next" => MusicNext
static void testEnglishNext()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand("next"), CabinCommand::MusicNext,
             "test 14: \"next\" must be MusicNext");
}

// "pause" => MusicPause
static void testEnglishPause()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand("pause"), CabinCommand::MusicPause,
             "test 15: \"pause\" must be MusicPause");
}

// "play music" => MusicPlay
static void testEnglishPlayMusic()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand("play music"), CabinCommand::MusicPlay,
             "test 16: \"play music\" must be MusicPlay");
}

// =========================================================================
// 冲突避免
// =========================================================================

// "暂停播放" => MusicPause（不是 MusicPlay，尽管包含 "播放" 子串）
static void testPausePlaybackConflict()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u6682\u505c\u64ad\u653e"), CabinCommand::MusicPause,
             "test 17: \"\u6682\u505c\u64ad\u653e\" must be MusicPause not MusicPlay");
}

// "打开音乐" => MusicShow（不是 MusicPlay）
static void testOpenMusicConflict()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u6253\u5f00\u97f3\u4e50"), CabinCommand::MusicShow,
             "test 18: \"\u6253\u5f00\u97f3\u4e50\" must be MusicShow not MusicPlay");
}

// =========================================================================
// 参数化指令（parseCabinCommandEx 返回 ParseResult 含 param）
// =========================================================================

// Convenience macro to check both command and param in one shot.
// Avoids duplicating the failure-printing boilerplate per test.
#define CHECK_RESULT(actual_cmd, actual_param, expected_cmd, expected_param, msg) do { \
    if ((actual_cmd) == (expected_cmd) && (actual_param) == (expected_param)) { \
        ++g_passCount; \
    } else { \
        ++g_failCount; \
        std::cerr << "FAIL: " << (msg) \
                  << " (got cmd=" << vehiclecommand::toString(actual_cmd) \
                  << ", param=" << (actual_param) \
                  << "; expected cmd=" << vehiclecommand::toString(expected_cmd) \
                  << ", param=" << (expected_param) << ")" << std::endl; \
    } \
} while(0)

// "放大 3 倍" => MapZoomIn, param=3
// 放=\u653e 大=\u5927 倍=\u500d
static void testZoomInWithMultiplier()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommandEx;
    auto r = parseCabinCommandEx(u8"\u653e\u5927 3 \u500d");
    CHECK_RESULT(r.command, r.param, CabinCommand::MapZoomIn, 3,
                 "test 19: \"\u653e\u5927 3 \u500d\" must be MapZoomIn param=3");
}

// "缩小 2 倍" => MapZoomOut, param=2
// 缩=\u7f29 小=\u5c0f
static void testZoomOutWithMultiplier()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommandEx;
    auto r = parseCabinCommandEx(u8"\u7f29\u5c0f 2 \u500d");
    CHECK_RESULT(r.command, r.param, CabinCommand::MapZoomOut, 2,
                 "test 20: \"\u7f29\u5c0f 2 \u500d\" must be MapZoomOut param=2");
}

// "放大 100 倍" => MapZoomIn, param=10 (clamped to [1,10])
static void testZoomClampedHigh()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommandEx;
    auto r = parseCabinCommandEx(u8"\u653e\u5927 100 \u500d");
    CHECK_RESULT(r.command, r.param, CabinCommand::MapZoomIn, 10,
                 "test 21: \"\u653e\u5927 100 \u500d\" must clamp to param=10");
}

// "放大地图" (no multiplier suffix) => MapZoomIn, param=0
// 地=\u5730 图=\u56fe
static void testZoomNoMultiplier()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommandEx;
    auto r = parseCabinCommandEx(u8"\u653e\u5927\u5730\u56fe");
    CHECK_RESULT(r.command, r.param, CabinCommand::MapZoomIn, 0,
                 "test 22: \"\u653e\u5927\u5730\u56fe\" must be MapZoomIn param=0");
}

// "放大地图 5 倍" => MapZoomIn, param=5 (multiplier extracted after synonym)
static void testZoomWithMapSuffix()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommandEx;
    auto r = parseCabinCommandEx(u8"\u653e\u5927\u5730\u56fe 5 \u500d");
    CHECK_RESULT(r.command, r.param, CabinCommand::MapZoomIn, 5,
                 "test 23: \"\u653e\u5927\u5730\u56fe 5 \u500d\" must be MapZoomIn param=5");
}

// "放大" alone (no 地图, no 倍) => Unknown, param=0
// "放大" is too short to be an actionable zoom command; the rule table
// requires "放大地图" or "地图放大" to fire MapZoomIn.
static void testZoomInAloneNoMatch()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommandEx;
    auto r = parseCabinCommandEx(u8"\u653e\u5927");
    CHECK_RESULT(r.command, r.param, CabinCommand::Unknown, 0,
                 "test 24: \"\u653e\u5927\" alone must be Unknown");
}

// =========================================================================
// 同义词表完整性回归（验证表驱动重构后所有同义词仍能匹配）
// =========================================================================

// "下一曲" => MusicNext (synonym of "下一首")
// 曲=\u66f2
static void testSynonymNextSongVariant()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u4e0b\u4e00\u66f2"), CabinCommand::MusicNext,
             "test 25: \"\u4e0b\u4e00\u66f2\" must be MusicNext (synonym)");
}

// "上一曲" => MusicPrevious
// 上=\u4e0a
static void testSynonymPreviousSongVariant()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u4e0a\u4e00\u66f2"), CabinCommand::MusicPrevious,
             "test 26: \"\u4e0a\u4e00\u66f2\" must be MusicPrevious (synonym)");
}

// "开始播放" => MusicPlay (synonym of "播放音乐")
// 开=\u5f00 始=\u59cb
static void testSynonymPlaybackStart()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u5f00\u59cb\u64ad\u653e"), CabinCommand::MusicPlay,
             "test 27: \"\u5f00\u59cb\u64ad\u653e\" must be MusicPlay (synonym)");
}

// "音乐界面" => MusicShow (synonym of "打开音乐")
// 界=\u754c 面=\u9762
static void testSynonymMusicPage()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u97f3\u4e50\u754c\u9762"), CabinCommand::MusicShow,
             "test 28: \"\u97f3\u4e50\u754c\u9762\" must be MusicShow (synonym)");
}

// "关闭音乐" => MusicClose (synonym of "退出音乐")
// 关=\u5173 闭=\u95ed
static void testSynonymMusicTurnOff()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand(u8"\u5173\u95ed\u97f3\u4e50"), CabinCommand::MusicClose,
             "test 29: \"\u5173\u95ed\u97f3\u4e50\" must be MusicClose (synonym)");
}

// "prev" => MusicPrevious (English short form)
static void testEnglishPrev()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommand;
    CHECK_EQ(parseCabinCommand("prev"), CabinCommand::MusicPrevious,
             "test 30: \"prev\" must be MusicPrevious (synonym)");
}

// "zoom in" => MapZoomIn, param=0 (English; no multiplier extraction)
static void testEnglishZoomIn()
{
    using vehiclecommand::CabinCommand;
    using vehiclecommand::parseCabinCommandEx;
    auto r = parseCabinCommandEx("zoom in");
    CHECK_RESULT(r.command, r.param, CabinCommand::MapZoomIn, 0,
                 "test 31: \"zoom in\" must be MapZoomIn param=0");
}

int main()
{
    testPlayAloneNoMatch();      // 1  负向（歧义文本拒绝执行）
    testOpenXiangMonitor();      // 2  正向
    testPlayerNoMatch();         // 3  负向
    testPlayListNoMatch();       // 4  负向
    testXiangAloneNoMatch();     // 5  负向
    testOpenChaosNoMatch();      // 6  负向
    testPlayMusic();             // 7  回归
    testOpenMap();               // 8  回归
    testViewMap();               // 9  回归
    testOpenCamera();            // 10 回归
    testViewCamera();            // 11 回归
    testNextSong();              // 12 回归
    testViewWeather();           // 13 回归
    testEnglishNext();           // 14 英文
    testEnglishPause();          // 15 英文
    testEnglishPlayMusic();      // 16 英文
    testPausePlaybackConflict(); // 17 冲突避免
    testOpenMusicConflict();     // 18 冲突避免

    testZoomInWithMultiplier();      // 19 参数化（放大 N 倍）
    testZoomOutWithMultiplier();      // 20 参数化（缩小 N 倍）
    testZoomClampedHigh();            // 21 参数化（clamp 10）
    testZoomNoMultiplier();           // 22 参数化（无后缀）
    testZoomWithMapSuffix();          // 23 参数化（同义词 + 倍数）
    testZoomInAloneNoMatch();         // 24 参数化（孤立"放大"=Unknown）
    testSynonymNextSongVariant();     // 25 同义词完整性
    testSynonymPreviousSongVariant(); // 26 同义词完整性
    testSynonymPlaybackStart();       // 27 同义词完整性
    testSynonymMusicPage();           // 28 同义词完整性
    testSynonymMusicTurnOff();        // 29 同义词完整性
    testEnglishPrev();                // 30 同义词完整性（英文）
    testEnglishZoomIn();              // 31 参数化（英文 + param=0）

    std::cout << "vehiclecommand tests: " << g_passCount << " passed, "
              << g_failCount << " failed" << std::endl;

    return g_failCount > 0 ? 1 : 0;
}
