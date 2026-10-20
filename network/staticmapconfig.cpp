#include "staticmapconfig.h"

#include <cctype>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace vehiclemap {
namespace {

std::string trim(const std::string &value)
{
    std::string::size_type first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }

    std::string::size_type last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }

    return value.substr(first, last - first);
}

bool startsWith(const std::string &value, const std::string &prefix)
{
    return value.compare(0, prefix.size(), prefix) == 0;
}

void putKeyValue(std::map<std::string, std::string> *values,
                 const std::string &line,
                 int lineNumber)
{
    const std::string::size_type separator = line.find('=');
    if (separator == std::string::npos) {
        throw MapConfigError("invalid map config line " + std::to_string(lineNumber));
    }

    const std::string key = trim(line.substr(0, separator));
    const std::string value = trim(line.substr(separator + 1));
    if (key.empty()) {
        throw MapConfigError("empty map config key at line " + std::to_string(lineNumber));
    }
    if (values->find(key) != values->end()) {
        throw MapConfigError("duplicate map config key: " + key);
    }

    (*values)[key] = value;
}

bool parseBoolValue(const std::string &value, bool *parsedValue)
{
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        *parsedValue = true;
        return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off") {
        *parsedValue = false;
        return true;
    }
    return false;
}

int parsePositiveInt(const std::string &value, const std::string &key)
{
    char *end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed <= 0 ||
        parsed > static_cast<long>(INT_MAX)) {
        throw MapConfigError("invalid positive integer for " + key);
    }
    return static_cast<int>(parsed);
}

int parseNonNegativeInt(const std::string &value, const std::string &key)
{
    char *end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed < 0 ||
        parsed > static_cast<long>(INT_MAX)) {
        throw MapConfigError("invalid non-negative integer for " + key);
    }
    return static_cast<int>(parsed);
}

void rejectUnknownKeys(const std::map<std::string, std::string> &values)
{
    static const char *knownKeys[] = {
        "map_enabled",
        "map_url_template",
        "map_api_key",
        "map_default_center",
        "map_default_zoom",
        "map_min_zoom",
        "map_max_zoom",
        "map_width",
        "map_height",
        "map_scale",
        "map_copyright",
        "map_markers_enabled",
        "map_output_file"
    };

    for (std::map<std::string, std::string>::const_iterator it = values.begin();
         it != values.end();
         ++it) {
        bool known = false;
        for (std::size_t index = 0; index < sizeof(knownKeys) / sizeof(knownKeys[0]); ++index) {
            if (it->first == knownKeys[index]) {
                known = true;
                break;
            }
        }
        if (!known) {
            throw MapConfigError("unknown map config key: " + it->first);
        }
    }
}

// Placeholder markers committed in example/template configs. A map_api_key
// still equal to one of these is treated as "not configured" so requests are
// never built from template values.
bool isPlaceholderApiKey(const std::string &value)
{
    return value == "YOUR_BAIDU_MAP_KEY" ||
           value == "YOUR_WEATHER_APP_ID" ||
           value == "YOUR_WEATHER_APP_SECRET" ||
           value == "YOUR_ASR_CUID" ||
           value == "YOUR_ASR_TOKEN";
}

void replaceAll(std::string *value, const std::string &from, const std::string &to)
{
    if (from.empty()) {
        return;
    }

    std::string::size_type position = 0;
    while ((position = value->find(from, position)) != std::string::npos) {
        value->replace(position, from.size(), to);
        position += to.size();
    }
}

void validateZoomRange(const StaticMapConfig &config)
{
    if (config.minZoom > config.maxZoom) {
        throw MapConfigError("map_min_zoom cannot be greater than map_max_zoom");
    }
    if (config.defaultZoom < config.minZoom || config.defaultZoom > config.maxZoom) {
        throw MapConfigError("map_default_zoom must be inside min/max range");
    }
}

} // namespace

MapConfigError::MapConfigError(const std::string &message)
    : std::runtime_error(message)
{
}

StaticMapConfig::StaticMapConfig()
    : enabled(true),
      urlTemplate(),
      apiKey(),
      defaultCenter("123.43147,41.80570"),
      defaultZoom(10),
      minZoom(3),
      maxZoom(18),
      width(1024),
      height(600),
      scale(1),
      copyright(1),
      markersEnabled(true),
      outputFile("map.png")
{
}

StaticMapConfig parseStaticMapConfigText(const std::string &configText)
{
    std::map<std::string, std::string> values;
    std::istringstream stream(configText);
    std::string line;
    int lineNumber = 0;

    while (std::getline(stream, line)) {
        ++lineNumber;
        const std::string trimmedLine = trim(line);
        if (trimmedLine.empty() || startsWith(trimmedLine, "#")) {
            continue;
        }
        putKeyValue(&values, trimmedLine, lineNumber);
    }

    rejectUnknownKeys(values);

    StaticMapConfig config;
    std::map<std::string, std::string>::const_iterator found =
        values.find("map_enabled");
    if (found != values.end() &&
        !parseBoolValue(found->second, &config.enabled)) {
        throw MapConfigError("invalid boolean for map_enabled");
    }

    found = values.find("map_url_template");
    if (found != values.end()) {
        config.urlTemplate = found->second;
    }

    found = values.find("map_api_key");
    if (found != values.end()) {
        config.apiKey = found->second;
    }

    found = values.find("map_default_center");
    if (found != values.end()) {
        config.defaultCenter = found->second;
    }

    found = values.find("map_default_zoom");
    if (found != values.end()) {
        config.defaultZoom = parsePositiveInt(found->second, "map_default_zoom");
    }

    found = values.find("map_min_zoom");
    if (found != values.end()) {
        config.minZoom = parsePositiveInt(found->second, "map_min_zoom");
    }

    found = values.find("map_max_zoom");
    if (found != values.end()) {
        config.maxZoom = parsePositiveInt(found->second, "map_max_zoom");
    }

    found = values.find("map_width");
    if (found != values.end()) {
        config.width = parsePositiveInt(found->second, "map_width");
    }

    found = values.find("map_height");
    if (found != values.end()) {
        config.height = parsePositiveInt(found->second, "map_height");
    }

    found = values.find("map_scale");
    if (found != values.end()) {
        config.scale = parsePositiveInt(found->second, "map_scale");
    }

    found = values.find("map_copyright");
    if (found != values.end()) {
        config.copyright = parseNonNegativeInt(found->second, "map_copyright");
    }

    found = values.find("map_markers_enabled");
    if (found != values.end() &&
        !parseBoolValue(found->second, &config.markersEnabled)) {
        throw MapConfigError("invalid boolean for map_markers_enabled");
    }

    found = values.find("map_output_file");
    if (found != values.end()) {
        config.outputFile = found->second;
    }

    if (config.defaultCenter.empty()) {
        throw MapConfigError("map_default_center cannot be empty");
    }
    if (config.outputFile.empty()) {
        throw MapConfigError("map_output_file cannot be empty");
    }
    // A disabled config may keep template placeholders committed to the repo.
    // Reject placeholders only when the network path is explicitly enabled.
    if (config.enabled && isPlaceholderApiKey(config.apiKey)) {
        throw MapConfigError(
            "map_api_key is still a placeholder (YOUR_BAIDU_MAP_KEY); "
            "set map_enabled=false or provide a real key");
    }
    validateZoomRange(config);

    return config;
}

StaticMapConfig loadStaticMapConfigFile(const std::string &path,
                                        bool *loaded,
                                        std::string *errorMessage)
{
    if (loaded != nullptr) {
        *loaded = false;
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        if (errorMessage != nullptr) {
            *errorMessage = "map config file not found: " + path;
        }
        return StaticMapConfig();
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    // parseStaticMapConfigText 可能抛出 MapConfigError(配置格式错误)
    // 或 std::bad_alloc 等系统异常。本函数设计通过 loaded/errorMessage
    // 报告错误而非异常,因此捕获所有 std::exception 并转为错误消息,
    // 防止异常逃逸到调用者。
    try {
        StaticMapConfig config = parseStaticMapConfigText(buffer.str());
        if (loaded != nullptr) {
            *loaded = true;
        }
        return config;
    } catch (const std::exception &e) {
        if (errorMessage != nullptr) {
            *errorMessage = "map config parse error: " + std::string(e.what());
        }
        return StaticMapConfig();
    }
}

std::string buildStaticMapUrl(const StaticMapConfig &config,
                              const std::string &center,
                              int zoom)
{
    if (!config.enabled) {
        return std::string();
    }
    if (config.apiKey.empty() || isPlaceholderApiKey(config.apiKey)) {
        return std::string();
    }

    std::string markerValue = center;
    if (!config.markersEnabled) {
        markerValue.clear();
    }

    std::string url = config.urlTemplate;
    replaceAll(&url, "{ak}", config.apiKey);
    replaceAll(&url, "{center}", center);
    replaceAll(&url, "{markers}", markerValue);
    replaceAll(&url, "{zoom}", std::to_string(zoom));
    replaceAll(&url, "{width}", std::to_string(config.width));
    replaceAll(&url, "{height}", std::to_string(config.height));
    replaceAll(&url, "{scale}", std::to_string(config.scale));
    replaceAll(&url, "{copyright}", std::to_string(config.copyright));
    return url;
}

} // namespace vehiclemap
