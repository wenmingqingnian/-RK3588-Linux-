#include "weatherconfig.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <map>

namespace vehicleweather {

WeatherConfigError::WeatherConfigError(const std::string &message)
    : std::runtime_error(message)
{
}

namespace {

std::string trim(const std::string &text)
{
    std::size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }
    std::size_t end = text.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(start, end - start);
}

std::string toLower(const std::string &text)
{
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

bool parseBoolValue(const std::string &key, const std::string &value)
{
    std::string lower = toLower(value);
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
        return true;
    }
    if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
        return false;
    }
    throw WeatherConfigError("invalid bool value for " + key + ": " + value);
}

int parsePositiveInt(const std::string &key, const std::string &value)
{
    if (value.empty()) {
        throw WeatherConfigError("empty value for " + key);
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(value[i])) == 0) {
            throw WeatherConfigError("non-numeric value for " + key + ": " + value);
        }
    }
    int result = 0;
    std::istringstream iss(value);
    iss >> result;
    if (result <= 0) {
        throw WeatherConfigError("non-positive value for " + key + ": " + value);
    }
    return result;
}

void replaceAll(std::string *text, const std::string &placeholder, const std::string &value)
{
    if (text == nullptr || placeholder.empty()) {
        return;
    }

    std::string::size_type pos = 0;
    while ((pos = text->find(placeholder, pos)) != std::string::npos) {
        text->replace(pos, placeholder.size(), value);
        pos += value.size();
    }
}

// Placeholder markers committed in example/template configs. Any credential
// still equal to one of these is treated as "not configured" so requests are
// never built from template values.
bool isPlaceholderOrEmpty(const std::string &value)
{
    if (value.empty()) {
        return true;
    }
    return value == "YOUR_WEATHER_APP_ID" ||
           value == "YOUR_WEATHER_APP_SECRET" ||
           value == "YOUR_BAIDU_MAP_KEY" ||
           value == "YOUR_ASR_CUID" ||
           value == "YOUR_ASR_TOKEN";
}

// Percent-encode a city name for safe URL substitution. Non-ASCII UTF-8 bytes
// and URL-special characters are encoded so a Chinese city (e.g. "沈阳") does
// not produce a malformed URL. ASCII alphanumerics and a few safe punctuation
// characters are kept as-is.
std::string urlEncodeCity(const std::string &city)
{
    std::string encoded;
    encoded.reserve(city.size() * 3);
    static const char *kHex = "0123456789ABCDEF";
    for (std::size_t i = 0; i < city.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(city[i]);
        if ((ch >= '0' && ch <= '9') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[ch >> 4]);
            encoded.push_back(kHex[ch & 0x0F]);
        }
    }
    return encoded;
}

void validateConfig(const WeatherConfig &config)
{
    if (!config.enabled) {
        return;
    }
    if (config.urlTemplate.empty()) {
        throw WeatherConfigError("weather_url_template is required when weather_enabled=true");
    }
    if (isPlaceholderOrEmpty(config.appId)) {
        throw WeatherConfigError(
            "weather_appid is missing or still a placeholder; "
            "set weather_enabled=false or provide a real appid");
    }
    if (isPlaceholderOrEmpty(config.appSecret)) {
        throw WeatherConfigError(
            "weather_appsecret is missing or still a placeholder; "
            "set weather_enabled=false or provide a real appsecret");
    }
}

} // namespace

WeatherConfig parseWeatherConfigText(const std::string &configText)
{
    WeatherConfig config;
    config.enabled = false;
    config.city = "";
    config.timeoutMs = 10000;

    std::map<std::string, bool> seen;
    std::istringstream stream(configText);
    std::string line;
    int lineNumber = 0;

    while (std::getline(stream, line)) {
        ++lineNumber;
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        std::size_t eqPos = trimmed.find('=');
        if (eqPos == std::string::npos) {
            throw WeatherConfigError("missing '=' at line " +
                                     std::to_string(lineNumber));
        }
        std::string key = trim(trimmed.substr(0, eqPos));
        std::string value = trim(trimmed.substr(eqPos + 1));

        if (seen.find(key) != seen.end()) {
            throw WeatherConfigError("duplicate key: " + key);
        }
        seen[key] = true;

        if (key == "weather_enabled") {
            config.enabled = parseBoolValue(key, value);
        } else if (key == "weather_url_template") {
            config.urlTemplate = value;
        } else if (key == "weather_appid") {
            config.appId = value;
        } else if (key == "weather_appsecret") {
            config.appSecret = value;
        } else if (key == "weather_city") {
            config.city = value;
        } else if (key == "weather_timeout_ms") {
            config.timeoutMs = parsePositiveInt(key, value);
        } else {
            throw WeatherConfigError("unknown key: " + key);
        }
    }

    validateConfig(config);
    return config;
}

WeatherConfig loadWeatherConfigFile(const std::string &path,
                                    bool *loaded,
                                    std::string *errorMessage)
{
    if (loaded != nullptr) {
        *loaded = false;
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        WeatherConfig config;
        config.enabled = false;
        config.city = "";
        config.timeoutMs = 10000;
        if (errorMessage != nullptr) {
            *errorMessage = "weather config file not found: " + path +
                            "; weather disabled";
        }
        return config;
    }

    std::ostringstream oss;
    oss << file.rdbuf();
    std::string content = oss.str();

    try {
        WeatherConfig config = parseWeatherConfigText(content);
        if (loaded != nullptr) {
            *loaded = true;
        }
        return config;
    } catch (const WeatherConfigError &e) {
        WeatherConfig config;
        config.enabled = false;
        config.timeoutMs = 10000;
        if (errorMessage != nullptr) {
            *errorMessage = "weather config parse error: " + std::string(e.what());
        }
        return config;
    }
}

std::string buildWeatherRequestUrl(const WeatherConfig &config)
{
    if (!config.enabled) {
        return std::string();
    }
    if (isPlaceholderOrEmpty(config.appId) || isPlaceholderOrEmpty(config.appSecret)) {
        return std::string();
    }

    std::string url = config.urlTemplate;

    replaceAll(&url, "{appid}", config.appId);
    replaceAll(&url, "{appsecret}", config.appSecret);
    // Percent-encode the city so a Chinese name (e.g. "沈阳") is not inserted
    // raw into the URL, which would produce a malformed request line.
    replaceAll(&url, "{city}", urlEncodeCity(config.city));

    return url;
}

} // namespace vehicleweather
