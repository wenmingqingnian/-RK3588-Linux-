#ifndef STATICMAPCONFIG_H
#define STATICMAPCONFIG_H

#include <stdexcept>
#include <string>

namespace vehiclemap {

class MapConfigError : public std::runtime_error {
public:
    explicit MapConfigError(const std::string &message);
};

struct StaticMapConfig {
    bool enabled;
    std::string urlTemplate;
    std::string apiKey;
    std::string defaultCenter;
    int defaultZoom;
    int minZoom;
    int maxZoom;
    int width;
    int height;
    int scale;
    int copyright;
    bool markersEnabled;
    std::string outputFile;

    StaticMapConfig();
};

StaticMapConfig parseStaticMapConfigText(const std::string &configText);
StaticMapConfig loadStaticMapConfigFile(const std::string &path,
                                        bool *loaded,
                                        std::string *errorMessage);
std::string buildStaticMapUrl(const StaticMapConfig &config,
                              const std::string &center,
                              int zoom);

} // namespace vehiclemap

#endif // STATICMAPCONFIG_H
