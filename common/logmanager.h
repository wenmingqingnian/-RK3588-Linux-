#ifndef VEHICLELOG_LOGMANAGER_H
#define VEHICLELOG_LOGMANAGER_H

#include <string>

namespace vehiclelog {

// Log levels ordered by severity.
// FATAL: system cannot continue, abort after logging.
// ERROR: module function failed, cannot recover automatically.
// WARN:  recoverable issue, module continues with degraded behavior.
// INFO:  normal lifecycle events (start, stop, config loaded).
// DEBUG: detailed internal state for development diagnostics.
enum class Level {
    Debug = 0,
    Info,
    Warn,
    Error,
    Fatal
};

const char *levelToString(Level level);
Level levelFromString(const std::string &name);

// Global log configuration.

// Set minimum log level. Messages below this level are discarded.
// Thread-safe: may be called concurrently with logMessage().
void setMinLevel(Level level);
Level minLevel();

// Set the active log file. The caller retains ownership of the FILE* and is
// responsible for closing it after logging is finished (typically after the
// application has shut down). LogManager only writes to it. Passing nullptr
// restores the default stderr destination.
// Thread-safe: may be called concurrently with logMessage().
void setLogFile(FILE *file);
FILE *logFile();

// Core log function. Prefer the macros below.
// format string position = 3, first variadic arg position = 4.
#if defined(__GNUC__) || defined(__clang__)
#  define VLOG_FORMAT_PRINTF(fmt_idx, arg_idx) \
       __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#  define VLOG_FORMAT_PRINTF(fmt_idx, arg_idx)
#endif
void logMessage(Level level, const char *module, const char *fmt, ...)
    VLOG_FORMAT_PRINTF(3, 4);

} // namespace vehiclelog

// Convenience macros. Module name is a string literal.
#define VLOG_DEBUG(module, fmt, ...) \
    vehiclelog::logMessage(vehiclelog::Level::Debug, module, fmt, ##__VA_ARGS__)

#define VLOG_INFO(module, fmt, ...) \
    vehiclelog::logMessage(vehiclelog::Level::Info, module, fmt, ##__VA_ARGS__)

#define VLOG_WARN(module, fmt, ...) \
    vehiclelog::logMessage(vehiclelog::Level::Warn, module, fmt, ##__VA_ARGS__)

#define VLOG_ERROR(module, fmt, ...) \
    vehiclelog::logMessage(vehiclelog::Level::Error, module, fmt, ##__VA_ARGS__)

#define VLOG_FATAL(module, fmt, ...) \
    vehiclelog::logMessage(vehiclelog::Level::Fatal, module, fmt, ##__VA_ARGS__)

#endif // VEHICLELOG_LOGMANAGER_H
