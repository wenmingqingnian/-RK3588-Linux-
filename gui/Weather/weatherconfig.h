#ifndef WEATHERCONFIG_H
#define WEATHERCONFIG_H

#include <stdexcept>
#include <string>

namespace vehicleweather {

struct WeatherConfig {
    bool enabled;
    std::string urlTemplate;
    std::string appId;
    std::string appSecret;
    std::string city;
    int timeoutMs;
};

class WeatherConfigError : public std::runtime_error {
public:
    explicit WeatherConfigError(const std::string &message);
};

WeatherConfig parseWeatherConfigText(const std::string &configText);

WeatherConfig loadWeatherConfigFile(const std::string &path,
                                    bool *loaded,
                                    std::string *errorMessage);

std::string buildWeatherRequestUrl(const WeatherConfig &config);

} // namespace vehicleweather

#endif // WEATHERCONFIG_H
