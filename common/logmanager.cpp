#include "logmanager.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

namespace vehiclelog {

static std::atomic<Level> g_minLevel{Level::Info};
static std::atomic<FILE *> g_logFile{nullptr};
static std::mutex g_logMutex;

const char *levelToString(Level level)
{
    switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO ";
    case Level::Warn:  return "WARN ";
    case Level::Error: return "ERROR";
    case Level::Fatal: return "FATAL";
    }
    return "?????";
}

Level levelFromString(const std::string &name)
{
    if (name == "DEBUG" || name == "debug") return Level::Debug;
    if (name == "INFO"  || name == "info")  return Level::Info;
    if (name == "WARN"  || name == "warn")  return Level::Warn;
    if (name == "ERROR" || name == "error") return Level::Error;
    if (name == "FATAL" || name == "fatal") return Level::Fatal;
    return Level::Info;
}

void setMinLevel(Level level)
{
    g_minLevel.store(level, std::memory_order_relaxed);
}

Level minLevel()
{
    return g_minLevel.load(std::memory_order_relaxed);
}

void setLogFile(FILE *file)
{
    g_logFile.store(file, std::memory_order_relaxed);
}

FILE *logFile()
{
    FILE *f = g_logFile.load(std::memory_order_relaxed);
    return f ? f : stderr;
}

void logMessage(Level level, const char *module, const char *fmt, ...)
{
    if (level < g_minLevel.load(std::memory_order_relaxed))
        return;

    std::lock_guard<std::mutex> lock(g_logMutex);

    FILE *out = g_logFile.load(std::memory_order_relaxed);
    if (!out)
        out = stderr;

    // Timestamp: HH:MM:SS
    time_t now = time(nullptr);
    char timeBuf[16] = "??:??:??";
    if (now != static_cast<time_t>(-1)) {
        struct tm tmBuf;
#ifdef _WIN32
        if (localtime_s(&tmBuf, &now) == 0)
#else
        if (localtime_r(&now, &tmBuf) != nullptr)
#endif
        {
            if (strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tmBuf) == 0)
                std::strcpy(timeBuf, "??:??:??");
        }
    }

    // [TIME][LEVEL][module] message
    // Defense against caller passing nullptr for module — printf "%s" with
    //nullptr is UB (glibc's "(null)" display is implementation-defined).
    fprintf(out, "[%s][%s][%s] ", timeBuf, levelToString(level),
            module ? module : "?");

    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fprintf(out, "\n");
    fflush(out);
}

} // namespace vehiclelog
