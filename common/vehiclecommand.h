#ifndef VEHICLECOMMAND_H
#define VEHICLECOMMAND_H

#include <string>

namespace vehiclecommand {

enum class CabinCommand {
    Unknown = 0,
    MusicShow,
    MusicPlay,
    MusicNext,
    MusicPrevious,
    MusicPause,
    MusicClose,
    MapShow,
    MapZoomIn,
    MapZoomOut,
    MonitorShow,
    WeatherShow
};

/**
 * @brief Parsed command result with optional numeric parameter.
 *
 * For commands that carry a quantitative argument (e.g. "放大 3 倍"),
 * `param` holds the extracted integer (3 in this example). For commands
 * without a parameter, `param` stays 0.
 *
 * Backward compatibility: parseCabinCommand() returns only the .command
 * field of this struct, so existing callers (MainWindow::dispatchVoiceCommand)
 * are unaffected by the introduction of parametric parsing.
 */
struct ParseResult {
    CabinCommand command = CabinCommand::Unknown;
    int param = 0;  // extracted integer argument; 0 when none present
};

/**
 * @brief Parse natural-language text into a cabin command (legacy API).
 *
 * Convenience wrapper around parseCabinCommandEx() that returns only the
 * command enum, discarding any numeric parameter. Existing call sites
 * (MainWindow::dispatchVoiceCommand) use this to avoid carrying the
 * ParseResult through their switch statements.
 */
CabinCommand parseCabinCommand(const std::string &text);

/**
 * @brief Parse natural-language text into a cabin command with parameter.
 *
 * Synonym table is data-driven (see vehiclecommand.cpp kSimpleRules[]):
 * each command maps to a list of phrases tried in priority order. Order
 * matters — Pause is checked before Play so "暂停播放" (which contains
 * "播放") resolves to MusicPause, not MusicPlay.
 *
 * Parametric extraction currently supports MapZoomIn / MapZoomOut with
 * an optional "N 倍" suffix:
 *   "放大 3 倍"  -> MapZoomIn,  param=3
 *   "缩小地图"   -> MapZoomOut, param=0  (no suffix -> no param)
 * The multiplier is clamped to [1, 10]; out-of-range or missing digits
 * yield param=0 (caller treats it as "default zoom step").
 *
 * MonitorShow is kept outside the synonym table because it also uses
 * containsAll() matching ("打开" + "摄像头" with filler words between).
 *
 * Never throws. Returns ParseResult{Unknown, 0} on no match.
 */
ParseResult parseCabinCommandEx(const std::string &text);

const char *toString(CabinCommand command);

} // namespace vehiclecommand

#endif // VEHICLECOMMAND_H
