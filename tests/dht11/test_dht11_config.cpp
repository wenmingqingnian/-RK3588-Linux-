// Dht11Config 解析单元测试（mingw 可编译，无 POSIX 依赖）。
//
// 验证：
//   - dht11_enabled=true + dht11_sysfs_dir 为空 不再抛异常（P1-1 修复）
//   - dht11_enabled=true + dht11_sysfs_dir=/some/path 正常解析
//   - dht11_temp_file / dht11_humidity_file 已从白名单删除（未知键拒绝）
//   - dht11_disabled 时 sysfs_dir 为空也合法
//
// 编译（mingw 或 Linux g++，在项目根目录执行）：
//   g++ -std=c++17 -Wall -Wextra -Idevice
//       tests/dht11/test_dht11_config.cpp
//       device/vehicledeviceconfig.cpp
//       -o tests/dht11/test_dht11_config

#include "vehicledeviceconfig.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(condition)                                              \
    do {                                                             \
        if (!(condition)) {                                          \
            std::printf("FAIL [%s:%d]: %s\n",                        \
                        __FILE__, __LINE__, #condition);             \
            ++g_failures;                                            \
        }                                                            \
    } while (0)

#define CHECK_THROWS(expression)                                      \
    do {                                                             \
        bool thrown = false;                                         \
        try {                                                        \
            (void)(expression);                                      \
        } catch (...) {                                              \
            thrown = true;                                           \
        }                                                            \
        CHECK(thrown);                                               \
    } while (0)

#define CHECK_NOTHROW(expression)                                     \
    do {                                                             \
        try {                                                        \
            (void)(expression);                                      \
        } catch (...) {                                              \
            CHECK(!"unexpected exception");                          \
        }                                                            \
    } while (0)

// P1-1 核心验证：dht11_enabled=true + dht11_sysfs_dir 为空 必须能解析通过。
// 修复前此用例会抛 DeviceConfigError("dht11_sysfs_dir is required...")。
void testEnabledWithEmptySysfsDirIsAccepted()
{
    const std::string text =
        "dht11_enabled=true\n"
        "dht11_sysfs_dir=\n"
        "dht11_poll_interval_ms=2000\n";

    vehicledevice::VehicleDeviceConfig config;
    CHECK_NOTHROW(config = vehicledevice::parseDeviceConfigText(text));
    CHECK(config.dht11.enabled == true);
    CHECK(config.dht11.sysfsDir.empty());
    CHECK(config.dht11.pollIntervalMs == 2000);
}

// dht11_enabled=true 且完全不写 dht11_sysfs_dir（等价于空）也合法。
void testEnabledWithoutSysfsDirKeyIsAccepted()
{
    const std::string text =
        "dht11_enabled=true\n"
        "dht11_poll_interval_ms=2000\n";

    vehicledevice::VehicleDeviceConfig config;
    CHECK_NOTHROW(config = vehicledevice::parseDeviceConfigText(text));
    CHECK(config.dht11.enabled == true);
    CHECK(config.dht11.sysfsDir.empty());
}

// dht11_enabled=true + 显式 sysfs_dir 正常解析。
void testEnabledWithExplicitSysfsDir()
{
    const std::string text =
        "dht11_enabled=true\n"
        "dht11_sysfs_dir=/sys/bus/iio/devices/iio:device0\n"
        "dht11_poll_interval_ms=1500\n";

    vehicledevice::VehicleDeviceConfig config;
    CHECK_NOTHROW(config = vehicledevice::parseDeviceConfigText(text));
    CHECK(config.dht11.enabled == true);
    CHECK(config.dht11.sysfsDir == "/sys/bus/iio/devices/iio:device0");
    CHECK(config.dht11.pollIntervalMs == 1500);
}

// dht11_disabled 时 sysfs_dir 为空合法。
void testDisabledWithEmptySysfsDirIsAccepted()
{
    const std::string text =
        "dht11_enabled=false\n"
        "dht11_sysfs_dir=\n"
        "dht11_poll_interval_ms=2000\n";

    vehicledevice::VehicleDeviceConfig config;
    CHECK_NOTHROW(config = vehicledevice::parseDeviceConfigText(text));
    CHECK(config.dht11.enabled == false);
    CHECK(config.dht11.sysfsDir.empty());
}

// dht11_temp_file / dht11_humidity_file 必须被拒绝（已从白名单删除）。
void testRemovedKeysAreRejected()
{
    {
        const std::string text =
            "dht11_enabled=false\n"
            "dht11_temp_file=/sys/bus/iio/devices/iio:device0/in_temp_input\n";
        CHECK_THROWS(vehicledevice::parseDeviceConfigText(text));
    }
    {
        const std::string text =
            "dht11_enabled=false\n"
            "dht11_humidity_file=/sys/bus/iio/devices/iio:device0/in_humidityrelative_input\n";
        CHECK_THROWS(vehicledevice::parseDeviceConfigText(text));
    }
}

// dht11_enabled 缺省时默认为 false。
void testDefaultDisabled()
{
    const std::string text = "dht11_poll_interval_ms=2000\n";
    vehicledevice::VehicleDeviceConfig config;
    CHECK_NOTHROW(config = vehicledevice::parseDeviceConfigText(text));
    CHECK(config.dht11.enabled == false);
    CHECK(config.dht11.pollIntervalMs == 2000);
}

// 非法 dht11_enabled 值必须被拒绝。
void testInvalidEnabledRejected()
{
    const std::string text = "dht11_enabled=maybe\n";
    CHECK_THROWS(vehicledevice::parseDeviceConfigText(text));
}

// 非法 dht11_poll_interval_ms 值必须被拒绝。
void testInvalidPollIntervalRejected()
{
    const std::string text =
        "dht11_enabled=false\n"
        "dht11_poll_interval_ms=0\n";  // 必须 > 0
    CHECK_THROWS(vehicledevice::parseDeviceConfigText(text));
}

} // namespace

int main()
{
    testEnabledWithEmptySysfsDirIsAccepted();
    testEnabledWithoutSysfsDirKeyIsAccepted();
    testEnabledWithExplicitSysfsDir();
    testDisabledWithEmptySysfsDirIsAccepted();
    testRemovedKeysAreRejected();
    testDefaultDisabled();
    testInvalidEnabledRejected();
    testInvalidPollIntervalRejected();

    if (g_failures == 0) {
        std::puts("dht11 config tests passed");
        return 0;
    }

    std::printf("dht11 config tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
