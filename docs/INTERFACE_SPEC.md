# Interface Spec

## Purpose

This file records stable interfaces so future changes do not repeatedly reshape contracts.

If an interface changes, update this file in the same change.

## Common Command Interface

Header:

```text
rk3588s_migration/common/vehiclecommand.h
```

Namespace:

```cpp
namespace vehiclecommand
```

Enum:

```cpp
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
```

Functions:

```cpp
CabinCommand parseCabinCommand(const std::string &text);
const char *toString(CabinCommand command);
```

Rules:

- Parser must be GUI-independent.
- Parser must not access hardware.
- New command values must be appended where possible to reduce breakage.
- Any new command must be documented here and tested.

## Device Configuration Interface

Header:

```text
rk3588s_migration/device/vehicledeviceconfig.h
```

Namespace:

```cpp
namespace vehicledevice
```

Main structures:

```cpp
struct KeyInputConfig {
    bool enabled;
    std::string devicePath;
    int pressedValue;
    int pollIntervalMs;
};

struct GpsSerialConfig {
    bool enabled;
    std::string devicePath;
    int baudrate;
    int dataBits;
    char parity;
    int stopBits;
    int readTimeoutMs;
};

struct VehicleDeviceConfig {
    KeyInputConfig keyInput;
    GpsSerialConfig gps;
    Dht11Config dht11;
    Ap3216Config ap3216;
};
```

Functions:

```cpp
VehicleDeviceConfig parseDeviceConfigText(const std::string &configText);
VehicleDeviceConfig loadDeviceConfigFile(const std::string &path,
                                         bool *loaded,
                                         std::string *errorMessage);
```

Error type:

```cpp
class DeviceConfigError : public std::runtime_error
```

Rules:

- Unknown keys are rejected.
- Duplicate keys are rejected.
- Enabled hardware with empty device path is rejected.
- Missing config file returns default disabled config and a readable warning.

## Device Config File Keys

Current config file:

```text
IMX6ULL车载终端/VehicleTerminal/config/vehicle_devices.conf
```

Current keys:

Key input and GPS keys are listed below. Sensor-specific keys are documented in the DHT11 and AP3216 sections.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `key_input_enabled` | bool | `false` | Enable Linux key input source |
| `key_input_device` | string | empty | Key input device node |
| `key_input_pressed_value` | positive int | `240` | Legacy pressed value |
| `key_input_poll_interval_ms` | positive int | `500` | Qt timer poll interval |
| `gps_enabled` | bool | `false` | Enable GPS serial input |
| `gps_device` | string | empty | GPS UART/USB serial device |
| `gps_baudrate` | positive int | `9600` | GPS serial baudrate |
| `gps_data_bits` | int 5-8 | `8` | GPS serial data bits |
| `gps_parity` | `N`, `O`, or `E` | `N` | GPS serial parity |
| `gps_stop_bits` | `1` or `2` | `1` | GPS serial stop bits |
| `gps_read_timeout_ms` | positive int | `500` | GPS poll timeout |

Bool values accepted:

```text
true, 1, yes, on, false, 0, no, off
```

## Key Input Interface

Header:

```text
rk3588s_migration/device/keyinputdevice.h
```

Main behavior:

- Disabled key input is non-fatal.
- Invalid enabled path returns failure and error text.
- Unavailable input reads neutral value `0`.
- Device file is released by RAII/destructor.

GUI usage rule:

- GUI may start/stop timers based on `KeyInputDevice::isAvailable()`.
- GUI must not open `/dev/key` or any key device directly.

## GPS C Interface

Header:

```text
IMX6ULL车载终端/VehicleTerminal/Map/gps.h
```

Type:

```c
#define GPS_DEVICE_PATH_MAX 128

typedef struct gps_uart_config {
    int enabled;
    char device_path[GPS_DEVICE_PATH_MAX];
    unsigned int baudrate;
    unsigned char data_bits;
    char parity;
    unsigned char stop_bits;
    unsigned int read_timeout_ms;
} gps_uart_config_t;
```

Functions:

```c
void gps_uart_config_init(gps_uart_config_t *config);
int gps_init(void);
int gps_init_with_config(const gps_uart_config_t *config);
void gps_shutdown(void);
int gps_is_valid(void);
int analyseRawData(void);
int getGpsData(char *N_S_Flag, char *E_W_Flag, double *jingdu, double *weidu);
```

Rules:

- Prefer `gps_init_with_config()` for RK3588S migration.
- `gps_init()` exists only for legacy compatibility and initializes disabled defaults.
- GUI must call `gps_is_valid()` instead of reading global variables.
- GUI must call `getGpsData()` instead of reading internal GPS state.
- Call `gps_shutdown()` when the map object is destroyed.

## DHT11 Device Configuration Interface

Header:

```text
rk3588s_migration/device/vehicledeviceconfig.h
```

Namespace:

```cpp
namespace vehicledevice
```

DHT11 config structure:

```cpp
struct Dht11Config {
    bool enabled;
    std::string devicePath;
    int pollIntervalMs;
    int readTimeoutMs;

    Dht11Config();
};
```

DHT11 device adapter header:

```text
rk3588s_migration/device/dht11device.h
```

DHT11 device adapter interface:

```cpp
struct Dht11Reading {
    bool valid;
    double temperature;
    double humidity;
    std::string errorMessage;

    Dht11Reading();
};

class Dht11Device {
public:
    explicit Dht11Device(const Dht11Config &config);
    ~Dht11Device();

    bool openDevice(std::string *errorMessage);
    void closeDevice();
    bool isAvailable() const;
    Dht11Reading readTemperatureHumidity(std::string *errorMessage) const;
};
```

DHT11 Qt adapter header:

```text
IMX6ULL车载终端/VehicleTerminal/dht11.h
```

DHT11 Qt adapter interface:

```cpp
class Dht11Reader : public QObject {
    Q_OBJECT
public:
    explicit Dht11Reader(const vehicledevice::Dht11Config &config, QObject *parent = nullptr);
    ~Dht11Reader();

    bool startReading(std::string *errorMessage);
    void stopReading();
    bool isAvailable() const;

signals:
    void updateDht11Data(QString humidity, QString temperature);
};
```

Internal DHT11 Qt worker:

```text
IMX6ULL车载终端/VehicleTerminal/dht11worker.h
IMX6ULL车载终端/VehicleTerminal/dht11worker.cpp
```

Rules for the internal worker:

- `Dht11Worker` is not a public service interface.
- `Dht11Reader` owns the worker thread lifecycle.
- The worker owns the Qt polling timer and the `Dht11Device` instance.
- GUI code must only use `Dht11Reader` and the `updateDht11Data` signal.

Current config keys for DHT11:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `dht11_enabled` | bool | `false` | Enable DHT11 temperature/humidity sensor |
| `dht11_device` | string | empty | DHT11 device node path |
| `dht11_poll_interval_ms` | positive int | `2000` | Poll interval in milliseconds |
| `dht11_read_timeout_ms` | positive int | `500` | Read timeout in milliseconds |

Rules:

- Unknown keys are rejected.
- Duplicate keys are rejected.
- Enabled DHT11 with empty device path is rejected.
- Disabled DHT11 allows empty device path.
- Missing config file returns default disabled config.
- `dht11_read_timeout_ms` is enforced by the device adapter on Linux through `poll()` before reading.
- GUI must not open DHT11 device directly; use `Dht11Reader`.
- GUI must not synchronously read DHT11 data; polling runs in `Dht11Worker`.
- Default config disables DHT11 to avoid startup errors on boards without the device.

## AP3216 Device Configuration Interface

Header:

```text
rk3588s_migration/device/vehicledeviceconfig.h
```

Namespace:

```cpp
namespace vehicledevice
```

AP3216 config structure:

```cpp
struct Ap3216Config {
    bool enabled;
    std::string sysfsDir;
    std::string alsFile;
    std::string psFile;
    std::string irFile;
    int pollIntervalMs;

    Ap3216Config();
};
```

AP3216 device adapter header:

```text
rk3588s_migration/device/ap3216device.h
```

AP3216 device adapter interface:

```cpp
struct Ap3216Reading {
    bool valid;
    std::string als;
    std::string ps;
    std::string ir;
    std::string errorMessage;

    Ap3216Reading();
};

class Ap3216Device {
public:
    explicit Ap3216Device(const Ap3216Config &config);
    ~Ap3216Device();

    bool openDevice(std::string *errorMessage);
    void closeDevice();
    bool isAvailable() const;
    Ap3216Reading readSensorValues(std::string *errorMessage) const;
};
```

AP3216 Qt adapter header:

```text
IMX6ULL杞﹁浇缁堢/VehicleTerminal/Monitor/ap3216.h
```

AP3216 Qt adapter interface:

```cpp
class Ap3216c : public QObject {
    Q_OBJECT
public:
    explicit Ap3216c(const vehicledevice::Ap3216Config &config,
                     QObject *parent = nullptr);
    ~Ap3216c();

    bool startReading(std::string *errorMessage);
    void stopReading();
    bool isAvailable() const;

signals:
    void updateAp3216Data(QString als, QString ps, QString ir);
};
```

Internal AP3216 Qt worker:

```text
IMX6ULL杞﹁浇缁堢/VehicleTerminal/Monitor/ap3216worker.h
IMX6ULL杞﹁浇缁堢/VehicleTerminal/Monitor/ap3216worker.cpp
```

Current config keys for AP3216:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `ap3216_enabled` | bool | `false` | Enable AP3216 light/proximity/infrared sensor |
| `ap3216_sysfs_dir` | string | empty | AP3216 sysfs directory containing channel files |
| `ap3216_als_file` | string | `als` | Ambient light channel file name |
| `ap3216_ps_file` | string | `ps` | Proximity/distance channel file name |
| `ap3216_ir_file` | string | `ir` | Infrared channel file name |
| `ap3216_poll_interval_ms` | positive int | `500` | Poll interval in milliseconds |

Rules:

- Unknown keys are rejected.
- Duplicate keys are rejected.
- Enabled AP3216 with empty `ap3216_sysfs_dir` is rejected.
- Disabled AP3216 allows an empty sysfs directory.
- Enabled AP3216 requires non-empty ALS, PS, and IR channel file names.
- Missing config file returns default disabled config.
- GUI must not open AP3216 sysfs files directly; use `Ap3216c`.
- GUI must not synchronously poll AP3216; polling runs in `Ap3216Worker`.
- Legacy `readAlsData()`, `readPsData()`, and `readIrData()` remain as cached compatibility accessors and must not perform file I/O.
- Default config disables AP3216 to avoid startup errors on boards without the sysfs node.

## Static Map Config Interface

Header:

```text
rk3588s_migration/map/staticmapconfig.h
```

Namespace:

```cpp
namespace vehiclemap
```

Main structure:

```cpp
struct StaticMapConfig {
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
};
```

Functions:

```cpp
StaticMapConfig parseStaticMapConfigText(const std::string &configText);
StaticMapConfig loadStaticMapConfigFile(const std::string &path,
                                        bool *loaded,
                                        std::string *errorMessage);
std::string buildStaticMapUrl(const StaticMapConfig &config,
                              const std::string &center,
                              int zoom);
```

Error type:

```cpp
class MapConfigError : public std::runtime_error
```

Config file:

```text
IMX6ULL杞﹁浇缁堢/VehicleTerminal/config/map_settings.conf
```

Qt runtime override:

```text
VEHICLE_MAP_CONFIG=/path/to/map_settings.conf
```

Current keys:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `map_url_template` | string | empty | Static image URL template with placeholders |
| `map_api_key` | string | empty | Map provider API key |
| `map_default_center` | string | `123.43147,41.80570` | Coordinate fallback used when GPS is disabled or invalid |
| `map_default_zoom` | positive int | `10` | Startup zoom |
| `map_min_zoom` | positive int | `3` | Minimum zoom allowed by UI |
| `map_max_zoom` | positive int | `18` | Maximum zoom allowed by UI |
| `map_width` | positive int | `1024` | Static image width |
| `map_height` | positive int | `600` | Static image height |
| `map_scale` | positive int | `1` | Static image scale parameter |
| `map_copyright` | non-negative int | `1` | Static image copyright parameter |
| `map_markers_enabled` | bool | `true` | Whether `{markers}` is filled with the active center |
| `map_output_file` | string | `map.png` | File written before the Qt stylesheet is refreshed |

Supported placeholders:

```text
{ak}, {center}, {markers}, {zoom}, {width}, {height}, {scale}, {copyright}
```

Rules:

- Unknown keys are rejected.
- Duplicate keys are rejected.
- Zoom default must be inside min/max range.
- Empty default center and output file are rejected.
- Missing config file returns defaults and disables network requests unless URL/API key are supplied elsewhere.
- GUI code may call the map config interface, but URL construction must stay outside widget logic.

## Static Map Response Interface

Header:

```text
rk3588s_migration/map/staticmapresponse.h
```

Namespace:

```cpp
namespace vehiclemap
```

Functions:

```cpp
bool isSupportedStaticMapImageResponse(const std::string &contentType,
                                       const std::string &data);
std::string makeStaticMapResponsePreview(const std::string &data,
                                         std::size_t maxLength);
```

Rules:

- Accept only supported image content types with matching image magic bytes.
- Current supported image types are PNG and JPEG.
- Reject HTTP 200 JSON/text error bodies from map providers before writing `map.png`.
- Log a short sanitized preview for rejected non-image responses.

## Weather Date Format Interface

Header:

```text
rk3588s_migration/weather/weatherformat.h
```

Namespace:

```cpp
namespace vehicleweather
```

Function:

```cpp
bool formatIsoDateToMonthDay(const std::string &dateText,
                             std::string *formattedText);
```

Rules:

- Accept only fixed-width `yyyy-mm-dd` date text.
- On success, write display-safe `MM-DD` to `formattedText` and return `true`.
- On failure, write `N/A` when `formattedText` is not null and return `false`.
- The helper is GUI-independent and must not access Qt widgets, network, files, or hardware.

## Weather Configuration Interface

Header:

```text
rk3588s_migration/weather/weatherconfig.h
```

Namespace:

```cpp
namespace vehicleweather
```

Main structure:

```cpp
struct WeatherConfig {
    bool enabled;
    std::string urlTemplate;
    std::string appId;
    std::string appSecret;
    std::string city;
    int timeoutMs;
};
```

Functions:

```cpp
WeatherConfig parseWeatherConfigText(const std::string &configText);
WeatherConfig loadWeatherConfigFile(const std::string &path,
                                    bool *loaded,
                                    std::string *errorMessage);
std::string buildWeatherRequestUrl(const WeatherConfig &config);
```

Error type:

```cpp
class WeatherConfigError : public std::runtime_error
```

Config file:

```text
IMX6ULL车载终端/VehicleTerminal/config/weather_settings.conf
```

Qt runtime override:

```text
VEHICLE_WEATHER_CONFIG=/path/to/weather_settings.conf
```

Current keys:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `weather_enabled` | bool | `false` | Enable weather network requests |
| `weather_url_template` | string | empty | Weather API URL template with placeholders |
| `weather_appid` | string | empty | Weather provider app id |
| `weather_appsecret` | string | empty | Weather provider app secret |
| `weather_city` | string | empty | City name for weather query |
| `weather_timeout_ms` | positive int | `10000` | Network request timeout |

Supported placeholders:

```text
{appid}, {appsecret}, {city}
```

Replacement behavior:

- Every occurrence of a supported placeholder is replaced, not only the first occurrence.
- Disabled config always builds an empty request URL.

Bool values accepted:

```text
true, 1, yes, on, false, 0, no, off
```

Rules:

- Unknown keys are rejected.
- Duplicate keys are rejected.
- Enabled weather with empty url template, appid, or appsecret is rejected.
- Disabled weather allows empty appid and appsecret.
- Missing config file returns default disabled config and a readable warning.
- Qt Weather page skips refresh requests when config is disabled or URL construction fails.
- Qt 5.15+ runtime applies `weather_timeout_ms` through `QNetworkRequest::setTransferTimeout`.
- GUI code may call the weather config interface, but URL construction must stay outside widget logic.

## Settings GIF Scanner Interface

Header:

```text
rk3588s_migration/gui/settinggifscanner.h
```

Namespace:

```cpp
namespace vehiclesettings
```

Function:

```cpp
std::vector<std::string> collectGifFiles(const std::string &directoryPath);
```

Behavior:

- Returns sorted full paths for regular files with `.gif` extension.
- Extension matching is case-insensitive.
- Missing, unreadable, or empty directories return an empty vector.
- The function does not throw intentionally and does not log directly; the Qt caller decides how to display/log the empty state.

Rules:

- `SettingWindow` may call this helper, but must not index the returned vector before checking whether it is empty.
- `SettingWindow` must treat missing/empty `./MyGif` as non-fatal startup state.
- The helper must remain GUI-independent and must not depend on Qt types.

## Voice Gateway Config Interface (RETIRED 2026-07-18, task A2)

> **Historical only.** The `rk3588s_migration/voice_gateway/` skeleton is
> retired and absent from the current workspace. The active voice
> configuration interface lives in `RK3588S_CarTerminal/config/voice_settings.conf`
> (see `voice/voice_config.*` for the parser). The information below is
> retained for tracing the original Project B contract.

Historical config file:

```text
rk3588s_migration/voice_gateway/config/voice_gateway.example.conf
```

Historical known endpoints:

- `llm`
- `tts`
- `status`

Historical observed board startup log:

```text
[INFO] loaded voice gateway config: llm=tcp://127.0.0.1:6666, tts=tcp://127.0.0.1:7777, status=tcp://127.0.0.1:6677
```

Historical rules (the equivalent rules now apply to `voice/VoiceManager.*`):

- Transport is optional.
- ZeroMQ must be enabled explicitly with CMake option `VOICE_GATEWAY_ENABLE_ZMQ=ON`.
- Voice gateway must communicate through command/service interfaces, not direct GUI calls.
