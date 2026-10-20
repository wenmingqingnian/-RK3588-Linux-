// Unit tests for vehicleweather::WeatherConfig.
//
// Covers the security hardening required by the first hardening pass:
//   - placeholder appid/appsecret are rejected and disable requests
//   - enabled=true with empty appsecret cannot build a valid request URL
//   - Chinese city names are percent-encoded so the URL is not malformed
//
// Build (host, no Qt): g++ -std=c++17 -Wall -Wextra test_weather_config.cpp
//                      ../../gui/Weather/weatherconfig.cpp -o test_weather_config
// Run: ./test_weather_config

#include "../../gui/Weather/weatherconfig.h"

#include <cassert>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

#define CHECK_THROWS(expr) \
    do { \
        bool thrown = false; \
        try { (expr); } catch (const vehicleweather::WeatherConfigError &) { \
            thrown = true; \
        } catch (...) { } \
        if (!thrown) { \
            std::printf("FAIL [%s:%d]: expected WeatherConfigError not thrown: %s\n", \
                        __FILE__, __LINE__, #expr); \
            ++g_failures; \
        } \
    } while (0)

void testPlaceholderAppIdDisables()
{
    const std::string text =
        "weather_enabled=true\n"
        "weather_url_template=http://example.com/free/week?appid={appid}&appsecret={appsecret}&city={city}\n"
        "weather_appid=YOUR_WEATHER_APP_ID\n"
        "weather_appsecret=real_secret\n"
        "weather_city=Shenyang\n";
    CHECK_THROWS(vehicleweather::parseWeatherConfigText(text));

    // loadWeatherConfigFile wrapper (which catches and returns disabled) is
    // exercised in testMissingFileDisables.
}

void testPlaceholderAppSecretDisables()
{
    const std::string text =
        "weather_enabled=true\n"
        "weather_url_template=http://example.com/free/week?appid={appid}&appsecret={appsecret}&city={city}\n"
        "weather_appid=real_appid\n"
        "weather_appsecret=YOUR_WEATHER_APP_SECRET\n"
        "weather_city=Shenyang\n";
    CHECK_THROWS(vehicleweather::parseWeatherConfigText(text));
}

void testEnabledButEmptyAppSecretCannotBuildUrl()
{
    // Construct a config directly to exercise buildWeatherRequestUrl's
    // defense-in-depth: even if validation is bypassed, an empty appsecret
    // must yield an empty URL (no request is built).
    vehicleweather::WeatherConfig config;
    config.enabled = true;
    config.urlTemplate =
        "http://example.com/free/week?appid={appid}&appsecret={appsecret}&city={city}";
    config.appId = "real_appid";
    config.appSecret = "";  // empty
    config.city = "Shenyang";
    config.timeoutMs = 10000;
    const std::string url = vehicleweather::buildWeatherRequestUrl(config);
    CHECK(url.empty());
}

void testPlaceholderAppSecretCannotBuildUrl()
{
    vehicleweather::WeatherConfig config;
    config.enabled = true;
    config.urlTemplate =
        "http://example.com/free/week?appid={appid}&appsecret={appsecret}&city={city}";
    config.appId = "real_appid";
    config.appSecret = "YOUR_WEATHER_APP_SECRET";
    config.city = "Shenyang";
    config.timeoutMs = 10000;
    const std::string url = vehicleweather::buildWeatherRequestUrl(config);
    CHECK(url.empty());
}

void testDisabledConfigBuildsEmptyUrl()
{
    vehicleweather::WeatherConfig config;
    config.enabled = false;
    config.urlTemplate =
        "http://example.com/free/week?appid={appid}&appsecret={appsecret}&city={city}";
    config.appId = "real_appid";
    config.appSecret = "real_secret";
    config.city = "Shenyang";
    config.timeoutMs = 10000;
    CHECK(vehicleweather::buildWeatherRequestUrl(config).empty());
}

void testChineseCityIsPercentEncoded()
{
    vehicleweather::WeatherConfig config;
    config.enabled = true;
    config.urlTemplate =
        "http://example.com/free/week?appid={appid}&appsecret={appsecret}&city={city}";
    config.appId = "real_appid";
    config.appSecret = "real_secret";
    config.city = "\xE6\xB2\x88\xE9\x98\xB3";  // "沈阳" in UTF-8
    config.timeoutMs = 10000;
    const std::string url = vehicleweather::buildWeatherRequestUrl(config);
    // URL must NOT contain raw non-ASCII bytes; "沈阳" UTF-8 is E6 B2 88 E9 98 B3
    // and must be encoded as %E6%B2%88%E9%98%B3.
    CHECK(url.find("%E6%B2%88%E9%98%B3") != std::string::npos);
    CHECK(url.find('\xE6') == std::string::npos);
    CHECK(url.find('\xE9') == std::string::npos);
}

void testAsciiCityNotNeedlesslyEncoded()
{
    vehicleweather::WeatherConfig config;
    config.enabled = true;
    config.urlTemplate =
        "http://example.com/free/week?appid={appid}&appsecret={appsecret}&city={city}";
    config.appId = "real_appid";
    config.appSecret = "real_secret";
    config.city = "Shenyang";
    config.timeoutMs = 10000;
    const std::string url = vehicleweather::buildWeatherRequestUrl(config);
    CHECK(url.find("city=Shenyang") != std::string::npos);
}

void testValidConfigBuildsUrl()
{
    const std::string text =
        "weather_enabled=true\n"
        "weather_url_template=http://example.com/free/week?appid={appid}&appsecret={appsecret}&city={city}\n"
        "weather_appid=real_appid\n"
        "weather_appsecret=real_secret\n"
        "weather_city=Shenyang\n"
        "weather_timeout_ms=8000\n";
    const vehicleweather::WeatherConfig config =
        vehicleweather::parseWeatherConfigText(text);
    CHECK(config.enabled);
    CHECK(config.appId == "real_appid");
    CHECK(config.appSecret == "real_secret");
    CHECK(config.timeoutMs == 8000);
    const std::string url = vehicleweather::buildWeatherRequestUrl(config);
    CHECK(!url.empty());
    CHECK(url.find("appid=real_appid") != std::string::npos);
    CHECK(url.find("appsecret=real_secret") != std::string::npos);
}

void testMissingFileDisables()
{
    bool loaded = true;
    std::string errorMessage;
    const vehicleweather::WeatherConfig config =
        vehicleweather::loadWeatherConfigFile(
            "nonexistent_weather_config_for_test.conf", &loaded, &errorMessage);
    CHECK(!loaded);
    CHECK(!config.enabled);
    CHECK(!errorMessage.empty());
}

} // namespace

int main()
{
    testPlaceholderAppIdDisables();
    testPlaceholderAppSecretDisables();
    testEnabledButEmptyAppSecretCannotBuildUrl();
    testPlaceholderAppSecretCannotBuildUrl();
    testDisabledConfigBuildsEmptyUrl();
    testChineseCityIsPercentEncoded();
    testAsciiCityNotNeedlesslyEncoded();
    testValidConfigBuildsUrl();
    testMissingFileDisables();

    if (g_failures == 0) {
        std::puts("weather_config tests passed");
        return 0;
    }
    std::printf("weather_config tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
