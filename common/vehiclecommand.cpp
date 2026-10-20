#include "vehiclecommand.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>

namespace vehiclecommand {
namespace {

const char *const kZhMusicNext = u8"下一首";
const char *const kZhMusicNextSong = u8"下一曲";
const char *const kZhMusicPrevious = u8"上一首";
const char *const kZhMusicPreviousSong = u8"上一曲";
const char *const kZhMusicPause = u8"暂停音乐";
const char *const kZhPlaybackPause = u8"暂停播放";
const char *const kZhMusicClose = u8"退出音乐";
const char *const kZhMusicTurnOff = u8"关闭音乐";
const char *const kZhMusicPlay = u8"播放音乐";
const char *const kZhPlaybackStart = u8"开始播放";
const char *const kZhMusicOpen = u8"打开音乐";
const char *const kZhMusicPage = u8"音乐界面";

const char *const kZhMapZoomIn = u8"地图放大";
const char *const kZhMapZoomInAlt = u8"放大地图";
const char *const kZhMapZoomOut = u8"地图缩小";
const char *const kZhMapZoomOutAlt = u8"缩小地图";
const char *const kZhMapOpen = u8"打开地图";
const char *const kZhMapShow = u8"显示地图";
const char *const kZhMapView = u8"查看地图";

// Parametric phrases for zoom commands. The standalone "放大"/"缩小" prefix
// enables matching "放大 N 倍" / "缩小 N 倍" — the digit N is extracted by
// extractZoomMultiplier() and stored in ParseResult::param.
// Keep these separate from kZhMapZoomIn/Alt above: "放大地图" contains both
// "放大" and "地图" and is already matched by kSimpleRules, so these prefixes
// are only consulted AFTER a zoom rule matches, to extract the multiplier.
const char *const kZhZoomInPrefix = u8"放大";
const char *const kZhZoomOutPrefix = u8"缩小";

const char *const kZhReverseStart = u8"开始倒车";
const char *const kZhReverseCamera = u8"倒车影像";
const char *const kZhCameraOpen = u8"打开摄像头";
const char *const kZhCameraShow = u8"显示摄像头";
const char *const kZhCameraView = u8"查看摄像头";
// Used for containsAll matching: "打开一下摄像头" splits across filler words.
const char *const kZhOpenVerb = u8"打开";
const char *const kZhCameraNoun = u8"摄像头";
// 变体写法 "摄象"（象，非 像）。仅与 "打开" 组合做 containsAll 匹配；
// "摄象" 单独出现不得触发，需同时包含 "打开"。
const char *const kZhCameraNounXiang = u8"摄象";

const char *const kZhMonitorShow = u8"打开监控";
const char *const kZhMonitorView = u8"显示监控";
const char *const kZhMonitorPage = u8"监控画面";

const char *const kZhWeatherQuery = u8"查看天气";
const char *const kZhWeatherHow = u8"天气怎么样";
const char *const kZhWeatherShow = u8"显示天气";
const char *const kZhWeatherToday = u8"今天天气";
const char *const kZhWeatherOpen = u8"打开天气";

char lowerAsciiChar(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return static_cast<char>(ch);
}

std::string lowerAscii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), lowerAsciiChar);
    return text;
}

bool containsAny(const std::string &text, std::initializer_list<const char *> phrases)
{
    for (const char *phrase : phrases) {
        if (text.find(phrase) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Returns true only when every phrase in `phrases` is present in `text`.
// Used to tolerate filler words inserted between a verb and a noun, e.g.
// "帮我打开一下摄像头" contains both "打开" and "摄像头".
bool containsAll(const std::string &text, std::initializer_list<const char *> phrases)
{
    for (const char *phrase : phrases) {
        if (text.find(phrase) == std::string::npos) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Synonym rule table (data-driven).
//
// Order is significant — the first matching rule wins. The table encodes
// the same priority the original if-cascade relied on:
//   - Pause must precede Play (so "暂停播放" containing "播放" resolves to
//     MusicPause, not MusicPlay).
//   - Close precedes Play for the same reason ("关闭音乐" should not fall
//     through to "音乐界面" / MusicShow).
//   - ZoomIn precedes MapShow ("地图放大" must not be eaten by "地图").
//
// MonitorShow is intentionally NOT in this table because it also relies on
// containsAll() matching (打开 + 摄像头 with filler words), which cannot
// be expressed as a single synonym list. It is handled separately after
// the table scan.
// ---------------------------------------------------------------------------
struct CommandRule {
    CabinCommand command;
    std::initializer_list<const char *> synonyms;
};

const CommandRule kSimpleRules[] = {
    { CabinCommand::MusicNext,     { kZhMusicNext, kZhMusicNextSong, "next" } },
    { CabinCommand::MusicPrevious, { kZhMusicPrevious, kZhMusicPreviousSong, "previous", "prev" } },
    { CabinCommand::MusicPause,    { kZhMusicPause, kZhPlaybackPause, "pause" } },
    { CabinCommand::MusicClose,    { kZhMusicClose, kZhMusicTurnOff, "close music" } },
    { CabinCommand::MusicPlay,     { kZhMusicPlay, kZhPlaybackStart, "play music" } },
    { CabinCommand::MusicShow,     { kZhMusicOpen, kZhMusicPage, "show music" } },
    { CabinCommand::MapZoomIn,     { kZhMapZoomIn, kZhMapZoomInAlt, "zoom in" } },
    { CabinCommand::MapZoomOut,    { kZhMapZoomOut, kZhMapZoomOutAlt, "zoom out" } },
    { CabinCommand::MapShow,       { kZhMapOpen, kZhMapShow, kZhMapView, "show map" } },
    { CabinCommand::WeatherShow,   { kZhWeatherQuery, kZhWeatherHow, kZhWeatherShow,
                                     kZhWeatherToday, kZhWeatherOpen, "show weather" } },
};

// ---------------------------------------------------------------------------
// extractZoomMultiplier: parse the integer N from "放大 N 倍" / "缩小 N 倍".
//
// Returns 0 when no digit is found (caller treats as "default zoom step").
// Returns a clamped value in [1, 10] otherwise — above 10x the UI zoom step
// would jump the map off-screen, and the user almost certainly meant a
// smaller multiplier.
//
// Implementation note: we only require the prefix ("放大" or "缩小") to be
// present somewhere in the text (loose containment), then scan the WHOLE
// text for the first ASCII digit run. This tolerates:
//   "放大 3 倍"     (space between prefix and digit)
//   "放大3倍"       (no space)
//   "放大地图 5 倍" (prefix + synonym + digit, all in one phrase)
//   "地图放大 N 倍" (synonym-then-prefix order)
// while still rejecting "放大地图" (no digit anywhere -> 0).
// ---------------------------------------------------------------------------
int extractZoomMultiplier(const std::string &text, const char *prefix)
{
    if (text.find(prefix) == std::string::npos) {
        return 0;
    }
    for (size_t i = 0; i < text.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(text[i]))) {
            int n = 0;
            size_t j = i;
            while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) {
                n = n * 10 + (text[j] - '0');
                ++j;
            }
            if (n < 1) return 1;
            if (n > 10) return 10;
            return n;
        }
    }
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// parseCabinCommandEx: data-driven parse with parametric extraction.
// ---------------------------------------------------------------------------
ParseResult parseCabinCommandEx(const std::string &text)
{
    ParseResult result;
    const std::string value = lowerAscii(text);

    for (const auto &rule : kSimpleRules) {
        if (containsAny(value, rule.synonyms)) {
            result.command = rule.command;
            // Parametric extraction for zoom commands: try the standalone
            // prefix first ("放大 N 倍"), then fall back to the alt phrase
            // ("放大地图 N 倍" is unusual but tolerated).
            if (rule.command == CabinCommand::MapZoomIn) {
                result.param = extractZoomMultiplier(value, kZhZoomInPrefix);
            } else if (rule.command == CabinCommand::MapZoomOut) {
                result.param = extractZoomMultiplier(value, kZhZoomOutPrefix);
            }
            return result;
        }
    }

    // MonitorShow: containsAny + containsAll fallback for filler-word
    // tolerance (e.g. "帮我打开一下摄像头" matches via containsAll on
    // {打开, 摄像头}).
    if (containsAny(value, {kZhReverseStart, kZhReverseCamera, kZhCameraOpen,
                            kZhCameraShow, kZhCameraView, kZhMonitorShow,
                            kZhMonitorView, kZhMonitorPage, "reverse camera",
                            "show monitor"}) ||
        containsAll(value, {kZhOpenVerb, kZhCameraNoun}) ||
        containsAll(value, {kZhOpenVerb, kZhCameraNounXiang})) {
        result.command = CabinCommand::MonitorShow;
        return result;
    }

    // Standalone "放大 N 倍" / "缩小 N 倍" pattern (no "地图" in text).
    // The table-driven rules above require "地图放大" or "放大地图", so
    // these phrases fall through to here. We accept them only when a digit
    // is actually present — bare "放大" alone must remain Unknown (test 24).
    {
        int zoomInParam = extractZoomMultiplier(value, kZhZoomInPrefix);
        if (zoomInParam > 0) {
            result.command = CabinCommand::MapZoomIn;
            result.param = zoomInParam;
            return result;
        }
        int zoomOutParam = extractZoomMultiplier(value, kZhZoomOutPrefix);
        if (zoomOutParam > 0) {
            result.command = CabinCommand::MapZoomOut;
            result.param = zoomOutParam;
            return result;
        }
    }

    return result;  // Unknown, param=0
}

CabinCommand parseCabinCommand(const std::string &text)
{
    return parseCabinCommandEx(text).command;
}

const char *toString(CabinCommand command)
{
    switch (command) {
    case CabinCommand::MusicShow:
        return "MusicShow";
    case CabinCommand::MusicPlay:
        return "MusicPlay";
    case CabinCommand::MusicNext:
        return "MusicNext";
    case CabinCommand::MusicPrevious:
        return "MusicPrevious";
    case CabinCommand::MusicPause:
        return "MusicPause";
    case CabinCommand::MusicClose:
        return "MusicClose";
    case CabinCommand::MapShow:
        return "MapShow";
    case CabinCommand::MapZoomIn:
        return "MapZoomIn";
    case CabinCommand::MapZoomOut:
        return "MapZoomOut";
    case CabinCommand::MonitorShow:
        return "MonitorShow";
    case CabinCommand::WeatherShow:
        return "WeatherShow";
    case CabinCommand::Unknown:
        return "Unknown";
    }

    // 防御性兜底：若未来新增 CabinCommand 枚举项而忘记在 toString 中添加
    // 对应 case，编译器（-Wswitch）会发出穷举警告。即便收到非法枚举值
    // （理论上不可达），std::abort 也保证 Debug/Release 两种构建下都立即
    // 终止进程并生成 core dump，避免静默返回 "Unknown" 引发上层误判。
    std::abort();
    return "Unknown"; // unreachable，仅满足 -Wreturn-type
}

} // namespace vehiclecommand
