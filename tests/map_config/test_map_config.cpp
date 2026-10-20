// Unit tests for vehiclemap::StaticMapConfig.
//
// Covers the security hardening required by the first hardening pass:
//   - placeholder map_api_key is rejected when map_enabled=true
//   - disabled template configs may keep YOUR_BAIDU_MAP_KEY placeholders
//   - enabled=true but key empty cannot build a valid request URL
//   - map_enabled=false disables URL building
//   - valid config still builds a URL with the key substituted
//
// Build (host, no Qt): g++ -std=c++17 -Wall -Wextra test_map_config.cpp
//                      ../../network/staticmapconfig.cpp -o test_map_config
// Run: ./test_map_config

#include "../../network/staticmapconfig.h"

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
        try { (expr); } catch (const vehiclemap::MapConfigError &) { \
            thrown = true; \
        } catch (...) { } \
        if (!thrown) { \
            std::printf("FAIL [%s:%d]: expected MapConfigError not thrown: %s\n", \
                        __FILE__, __LINE__, #expr); \
            ++g_failures; \
        } \
    } while (0)

void testPlaceholderApiKeyRejected()
{
    const std::string text =
        "map_enabled=true\n"
        "map_url_template=http://api.example.com/staticimage?ak={ak}&center={center}\n"
        "map_api_key=YOUR_BAIDU_MAP_KEY\n"
        "map_default_center=123.43147,41.80570\n"
        "map_default_zoom=10\n"
        "map_min_zoom=3\n"
        "map_max_zoom=18\n"
        "map_width=1024\n"
        "map_height=600\n"
        "map_scale=1\n"
        "map_copyright=1\n"
        "map_markers_enabled=true\n"
        "map_output_file=map.png\n";
    CHECK_THROWS(vehiclemap::parseStaticMapConfigText(text));
}

void testDisabledTemplatePlaceholderIsAccepted()
{
    const std::string text =
        "map_enabled=false\n"
        "map_url_template=http://api.example.com/staticimage?ak={ak}&center={center}\n"
        "map_api_key=YOUR_BAIDU_MAP_KEY\n"
        "map_default_center=123.43147,41.80570\n"
        "map_default_zoom=10\n"
        "map_min_zoom=3\n"
        "map_max_zoom=18\n"
        "map_width=1024\n"
        "map_height=600\n"
        "map_scale=1\n"
        "map_copyright=1\n"
        "map_markers_enabled=true\n"
        "map_output_file=map.png\n";

    const vehiclemap::StaticMapConfig config =
        vehiclemap::parseStaticMapConfigText(text);
    CHECK(!config.enabled);
    CHECK(config.apiKey == "YOUR_BAIDU_MAP_KEY");
    CHECK(vehiclemap::buildStaticMapUrl(config, "123.4,41.8", 10).empty());
}

void testEmptyApiKeyCannotBuildUrl()
{
    vehiclemap::StaticMapConfig config;
    config.enabled = true;
    config.urlTemplate = "http://api.example.com/staticimage?ak={ak}&center={center}";
    config.apiKey = "";  // empty
    config.defaultCenter = "123.43147,41.80570";
    config.defaultZoom = 10;
    config.minZoom = 3;
    config.maxZoom = 18;
    config.width = 1024;
    config.height = 600;
    config.scale = 1;
    config.copyright = 1;
    config.markersEnabled = true;
    config.outputFile = "map.png";
    const std::string url = vehiclemap::buildStaticMapUrl(config, "123.4,41.8", 10);
    CHECK(url.empty());
}

void testPlaceholderApiKeyCannotBuildUrl()
{
    // Defense in depth: even if a placeholder key somehow reaches
    // buildStaticMapUrl, no URL must be produced.
    vehiclemap::StaticMapConfig config;
    config.enabled = true;
    config.urlTemplate = "http://api.example.com/staticimage?ak={ak}&center={center}";
    config.apiKey = "YOUR_BAIDU_MAP_KEY";
    config.defaultCenter = "123.43147,41.80570";
    config.defaultZoom = 10;
    config.minZoom = 3;
    config.maxZoom = 18;
    config.width = 1024;
    config.height = 600;
    config.scale = 1;
    config.copyright = 1;
    config.markersEnabled = true;
    config.outputFile = "map.png";
    const std::string url = vehiclemap::buildStaticMapUrl(config, "123.4,41.8", 10);
    CHECK(url.empty());
}

void testDisabledFlagBuildsEmptyUrl()
{
    vehiclemap::StaticMapConfig config;
    config.enabled = false;
    config.urlTemplate = "http://api.example.com/staticimage?ak={ak}&center={center}";
    config.apiKey = "real_key";
    config.defaultCenter = "123.43147,41.80570";
    config.defaultZoom = 10;
    config.minZoom = 3;
    config.maxZoom = 18;
    config.width = 1024;
    config.height = 600;
    config.scale = 1;
    config.copyright = 1;
    config.markersEnabled = true;
    config.outputFile = "map.png";
    const std::string url = vehiclemap::buildStaticMapUrl(config, "123.4,41.8", 10);
    CHECK(url.empty());
}

void testValidConfigBuildsUrl()
{
    const std::string text =
        "map_enabled=true\n"
        "map_url_template=http://api.example.com/staticimage?ak={ak}&center={center}&zoom={zoom}\n"
        "map_api_key=real_key_value\n"
        "map_default_center=123.43147,41.80570\n"
        "map_default_zoom=10\n"
        "map_min_zoom=3\n"
        "map_max_zoom=18\n"
        "map_width=1024\n"
        "map_height=600\n"
        "map_scale=1\n"
        "map_copyright=1\n"
        "map_markers_enabled=true\n"
        "map_output_file=map.png\n";
    const vehiclemap::StaticMapConfig config =
        vehiclemap::parseStaticMapConfigText(text);
    CHECK(config.enabled);
    CHECK(config.apiKey == "real_key_value");
    const std::string url = vehiclemap::buildStaticMapUrl(config, "123.4,41.8", 12);
    CHECK(!url.empty());
    CHECK(url.find("ak=real_key_value") != std::string::npos);
    CHECK(url.find("center=123.4,41.8") != std::string::npos);
    CHECK(url.find("zoom=12") != std::string::npos);
}

void testEnabledDefaultsToTrueWhenAbsent()
{
    // Backward compatibility: a config without map_enabled must default to
    // enabled=true (so existing configs with a real key keep working).
    const std::string text =
        "map_url_template=http://api.example.com/staticimage?ak={ak}\n"
        "map_api_key=real_key_value\n"
        "map_default_center=123.43147,41.80570\n"
        "map_default_zoom=10\n"
        "map_min_zoom=3\n"
        "map_max_zoom=18\n"
        "map_width=1024\n"
        "map_height=600\n"
        "map_scale=1\n"
        "map_copyright=1\n"
        "map_markers_enabled=true\n"
        "map_output_file=map.png\n";
    const vehiclemap::StaticMapConfig config =
        vehiclemap::parseStaticMapConfigText(text);
    CHECK(config.enabled);
}

void testMissingFileReturnsDefaultConfig()
{
    bool loaded = true;
    std::string errorMessage;
    const vehiclemap::StaticMapConfig config =
        vehiclemap::loadStaticMapConfigFile(
            "nonexistent_map_config_for_test.conf", &loaded, &errorMessage);
    CHECK(!loaded);
    // Default config has empty apiKey -> buildStaticMapUrl returns empty.
    CHECK(config.apiKey.empty());
    CHECK(!errorMessage.empty());
    CHECK(vehiclemap::buildStaticMapUrl(config, "123.4,41.8", 10).empty());
}

} // namespace

int main()
{
    testPlaceholderApiKeyRejected();
    testDisabledTemplatePlaceholderIsAccepted();
    testEmptyApiKeyCannotBuildUrl();
    testPlaceholderApiKeyCannotBuildUrl();
    testDisabledFlagBuildsEmptyUrl();
    testValidConfigBuildsUrl();
    testEnabledDefaultsToTrueWhenAbsent();
    testMissingFileReturnsDefaultConfig();

    if (g_failures == 0) {
        std::puts("map_config tests passed");
        return 0;
    }
    std::printf("map_config tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
