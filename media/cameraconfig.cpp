#include "cameraconfig.h"
#include "logmanager.h"

#include <cstring>
#include <cctype>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <map>

static const char *MODULE = "camera.config";

namespace vehiclecamera {

CameraConfigError::CameraConfigError(const std::string &message)
    : std::runtime_error(message)
{
}

static std::string trim(const std::string &s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return std::string();
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static bool parseBool(const std::string &key, const std::string &value,
                      std::string *error)
{
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on")
        return true;
    if (lower == "false" || lower == "0" || lower == "no" || lower == "off")
        return false;
    if (error)
        *error = key + ": invalid boolean value '" + value + "'";
    return false;
}

static bool isSupportedFormat(const std::string &format)
{
    return format == "NV12" || format == "NV21" ||
           format == "UYVY" || format == "YUYV" || format == "MJPEG";
}

static int parseInt(const std::string &key, const std::string &value,
                    std::string *error)
{
    try {
        size_t pos = 0;
        int result = std::stoi(value, &pos);
        if (pos != value.size()) {
            if (error)
                *error = key + ": invalid integer value '" + value + "'";
            return 0;
        }
        return result;
    } catch (...) {
        if (error)
            *error = key + ": invalid integer value '" + value + "'";
        return 0;
    }
}

static bool isKnownKey(const std::string &key)
{
    static const std::string knownKeys[] = {
        "camera_enabled",
        "camera_device",
        "camera_width",
        "camera_height",
        "camera_format",
        "camera_buffer_count",
        "camera_fps",
        // Media source selection (task: software end-to-end loop).
        "media_source_type",
        "media_file_path",
        "media_file_loop",
        "media_queue_capacity",
        "media_start_timeout_ms",
        "media_read_timeout_ms",
        "media_stop_timeout_ms"
    };
    for (const auto &k : knownKeys) {
        if (k == key)
            return true;
    }
    return false;
}

CameraConfig parseCameraConfigText(const std::string &configText)
{
    CameraConfig config;
    std::map<std::string, bool> seen;
    std::string parseError;
    int lineNumber = 0;

    std::istringstream stream(configText);
    std::string line;

    while (std::getline(stream, line)) {
        ++lineNumber;
        std::string trimmed = trim(line);

        if (trimmed.empty() || trimmed[0] == '#')
            continue;

        size_t eqPos = trimmed.find('=');
        if (eqPos == std::string::npos) {
            throw CameraConfigError(
                "camera config line " + std::to_string(lineNumber) +
                ": missing '=' in '" + trimmed + "'");
        }

        std::string key = trim(trimmed.substr(0, eqPos));
        std::string value = trim(trimmed.substr(eqPos + 1));

        if (!isKnownKey(key)) {
            throw CameraConfigError(
                "camera config line " + std::to_string(lineNumber) +
                ": unknown key '" + key + "'");
        }

        if (seen.count(key)) {
            throw CameraConfigError(
                "camera config line " + std::to_string(lineNumber) +
                ": duplicate key '" + key + "'");
        }
        seen[key] = true;

        if (key == "camera_enabled") {
            parseError.clear();
            config.enabled = parseBool(key, value, &parseError);
            if (!parseError.empty())
                throw CameraConfigError(parseError);
        } else if (key == "camera_device") {
            config.device = value;
        } else if (key == "camera_width") {
            parseError.clear();
            config.width = parseInt(key, value, &parseError);
            if (!parseError.empty())
                throw CameraConfigError(parseError);
            if (config.width < 32 || config.width > 3840) {
                throw CameraConfigError(
                    "camera_width must be between 32 and 3840, got " +
                    std::to_string(config.width));
            }
        } else if (key == "camera_height") {
            parseError.clear();
            config.height = parseInt(key, value, &parseError);
            if (!parseError.empty())
                throw CameraConfigError(parseError);
            if (config.height < 32 || config.height > 2160) {
                throw CameraConfigError(
                    "camera_height must be between 32 and 2160, got " +
                    std::to_string(config.height));
            }
        } else if (key == "camera_format") {
            std::string upper = value;
            std::transform(upper.begin(), upper.end(), upper.begin(),
                           [](unsigned char ch) {
                               return static_cast<char>(std::toupper(ch));
                           });
            config.format = upper;
        } else if (key == "camera_buffer_count") {
            parseError.clear();
            config.bufferCount = parseInt(key, value, &parseError);
            if (!parseError.empty())
                throw CameraConfigError(parseError);
            if (config.bufferCount < 2 || config.bufferCount > 8) {
                throw CameraConfigError(
                    "camera_buffer_count must be between 2 and 8, got " +
                    std::to_string(config.bufferCount));
            }
        } else if (key == "camera_fps") {
            parseError.clear();
            config.fps = parseInt(key, value, &parseError);
            if (!parseError.empty())
                throw CameraConfigError(parseError);
            if (config.fps < 1 || config.fps > 120) {
                throw CameraConfigError(
                    "camera_fps must be between 1 and 120, got " +
                    std::to_string(config.fps));
            }
        } else if (key == "media_source_type") {
            std::string lower = value;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char ch) {
                               return static_cast<char>(std::tolower(ch));
                           });
            if (lower != "v4l2" && lower != "test_pattern" && lower != "file") {
                throw CameraConfigError(
                    "media_source_type must be 'v4l2', 'test_pattern', or 'file', got '" + value + "'");
            }
            config.sourceType = lower;
        } else if (key == "media_file_path") {
            config.filePath = value;
        } else if (key == "media_file_loop") {
            parseError.clear();
            config.fileLoop = parseBool(key, value, &parseError);
            if (!parseError.empty())
                throw CameraConfigError(parseError);
        } else if (key == "media_queue_capacity") {
            parseError.clear();
            config.queueCapacity = parseInt(key, value, &parseError);
            if (!parseError.empty())
                throw CameraConfigError(parseError);
            if (config.queueCapacity < 1 || config.queueCapacity > 16) {
                throw CameraConfigError(
                    "media_queue_capacity must be between 1 and 16, got " +
                    std::to_string(config.queueCapacity));
            }
        } else if (key == "media_start_timeout_ms") {
            parseError.clear();
            config.startTimeoutMs = parseInt(key, value, &parseError);
            if (!parseError.empty())
                throw CameraConfigError(parseError);
        } else if (key == "media_read_timeout_ms") {
            parseError.clear();
            config.readTimeoutMs = parseInt(key, value, &parseError);
            if (!parseError.empty())
                throw CameraConfigError(parseError);
        } else if (key == "media_stop_timeout_ms") {
            parseError.clear();
            config.stopTimeoutMs = parseInt(key, value, &parseError);
            if (!parseError.empty())
                throw CameraConfigError(parseError);
        }
    }

    if (!config.format.empty() && !isSupportedFormat(config.format)) {
        throw CameraConfigError("unsupported camera_format '" + config.format + "'");
    }
    if (!config.format.empty()) {
        const bool needsEvenWidth =
            config.format == "NV12" || config.format == "NV21" ||
            config.format == "UYVY" || config.format == "YUYV" ||
            config.format == "MJPEG";
        const bool needsEvenHeight =
            config.format == "NV12" || config.format == "NV21" ||
            config.format == "MJPEG";
        if ((needsEvenWidth && (config.width % 2) != 0) ||
            (needsEvenHeight && (config.height % 2) != 0)) {
            throw CameraConfigError(
                "camera dimensions are incompatible with format " + config.format);
        }
    }

    // Validate enabled config
    if (config.enabled) {
        if (config.sourceType == "v4l2") {
            if (config.device.empty()) {
                throw CameraConfigError(
                    "camera_enabled=true but camera_device is empty");
            }
            if (config.format.empty()) {
                throw CameraConfigError(
                    "camera_enabled=true but camera_format is empty");
            }
        } else if (config.sourceType == "file") {
            if (config.filePath.empty()) {
                throw CameraConfigError(
                    "media_source_type=file but media_file_path is empty");
            }
        }
        // test_pattern requires no additional fields.
    }

    VLOG_INFO(MODULE,
              "parsed: enabled=%d source=%s device=%s file=%s %dx%d fps=%d "
              "queue=%d timeouts=%d/%d/%dms",
              config.enabled, config.sourceType.c_str(),
              config.device.c_str(), config.filePath.c_str(),
              config.width, config.height, config.fps,
              config.queueCapacity,
              config.startTimeoutMs, config.readTimeoutMs,
              config.stopTimeoutMs);

    return config;
}

CameraConfig loadCameraConfigFile(const std::string &path,
                                  bool *loaded,
                                  std::string *errorMessage)
{
    CameraConfig config; // default: enabled=false

    std::ifstream file(path);
    if (!file.is_open()) {
        if (loaded) *loaded = false;
        if (errorMessage)
            *errorMessage = "camera config file not found: " + path;
        VLOG_WARN(MODULE, "config file not found: %s", path.c_str());
        return config;
    }

    if (loaded) *loaded = true;
    VLOG_DEBUG(MODULE, "loading config from %s", path.c_str());

    std::ostringstream content;
    content << file.rdbuf();
    file.close();

    try {
        config = parseCameraConfigText(content.str());
        if (errorMessage) errorMessage->clear();
    } catch (const CameraConfigError &e) {
        if (errorMessage) *errorMessage = e.what();
        VLOG_ERROR(MODULE, "config parse error: %s", e.what());
        // Return default disabled config on parse error
        config = CameraConfig();
    }

    return config;
}

} // namespace vehiclecamera
