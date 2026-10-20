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

    std::cout << "vehiclecommand tests: " << g_passCount << " passed, "
              << g_failCount << " failed" << std::endl;

    return g_failCount > 0 ? 1 : 0;
}
