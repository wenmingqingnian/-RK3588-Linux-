// Unit tests for GPS configuration parsing in vehicledevice::VehicleDeviceConfig.
//
// Build (host, no Qt):
//   g++ -std=c++17 -Wall -Wextra -Werror -I../../common -I../../device
//       test_gpsconfig.cpp ../../device/vehicledeviceconfig.cpp
//       ../../common/logmanager.cpp -o test_gpsconfig
// Run: ./test_gpsconfig
//
// Covers spec config tests 27-33:
//   27. Both real and simulation GPS default to false.
//   28. Enabling both modes simultaneously is rejected.
//   29. Real mode without a device path is rejected.
//   30. Simulation mode without a file is rejected.
//   31. An invalid baud rate is rejected.
//   32. An invalid timeout/interval is rejected.
//   33. (bonus) Duplicate keys are rejected per project convention.

#include "vehicledeviceconfig.h"

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
        try { (expr); } catch (const vehicledevice::DeviceConfigError &) { \
            thrown = true; \
        } catch (...) { } \
        if (!thrown) { \
            std::printf("FAIL [%s:%d]: expected DeviceConfigError not thrown: %s\n", \
                        __FILE__, __LINE__, #expr); \
            ++g_failures; \
        } \
    } while (0)

// 27. Default config parses both gps_enabled and gps_simulation_enabled as
//     false (and the default file content shipped to the board must also).
void test27_DefaultsBothFalse()
{
    // Minimal valid config: empty body. Defaults already enforce false/false.
    const std::string text;
    const vehicledevice::VehicleDeviceConfig config =
        vehicledevice::parseDeviceConfigText(text);
    CHECK(!config.gps.enabled);
    CHECK(!config.gpsSimulation.enabled);
    CHECK(config.gps.devicePath.empty());
    CHECK(config.gpsSimulation.file.empty());
    CHECK(config.gps.baudrate == 9600);
    CHECK(config.gps.readTimeoutMs > 0);
    CHECK(config.gps.updateIntervalMs > 0);
    CHECK(config.gpsSimulation.intervalMs > 0);
    CHECK(config.gpsSimulation.mapUpdateIntervalMs > 0);
}

// 27b. Explicit disabled config is also accepted (mirrors shipping config).
void test27b_ExplicitDisabledAccepted()
{
    const std::string text =
        "gps_enabled=false\n"
        "gps_simulation_enabled=false\n";
    const vehicledevice::VehicleDeviceConfig config =
        vehicledevice::parseDeviceConfigText(text);
    CHECK(!config.gps.enabled);
    CHECK(!config.gpsSimulation.enabled);
}

// 28. Both modes enabled simultaneously must be rejected.
void test28_BothEnabledRejected()
{
    const std::string text =
        "gps_enabled=true\n"
        "gps_device=/dev/ttyUSB0\n"
        "gps_simulation_enabled=true\n"
        "gps_simulation_file=tests/gps/data/simulated_route.nmea\n";
    CHECK_THROWS(vehicledevice::parseDeviceConfigText(text));
}

// 29. Real mode enabled but device path empty must be rejected.
void test29_RealModeEmptyDeviceRejected()
{
    const std::string text =
        "gps_enabled=true\n"
        "gps_device=\n";
    CHECK_THROWS(vehicledevice::parseDeviceConfigText(text));
}

// 30. Simulation mode enabled but file path empty must be rejected.
void test30_SimModeEmptyFileRejected()
{
    const std::string text =
        "gps_simulation_enabled=true\n"
        "gps_simulation_file=\n";
    CHECK_THROWS(vehicledevice::parseDeviceConfigText(text));
}

// 31. Invalid baud rate (not in allowed list) is rejected.
void test31_InvalidBaudrateRejected()
{
    const std::string text =
        "gps_enabled=true\n"
        "gps_device=/dev/ttyUSB0\n"
        "gps_baudrate=14400\n"; // 14400 is not a standard rate
    CHECK_THROWS(vehicledevice::parseDeviceConfigText(text));
}

// 31b. A valid baud rate from the allowed list is accepted.
void test31b_ValidBaudrateAccepted()
{
    const std::string text =
        "gps_enabled=false\n"
        "gps_baudrate=115200\n";
    const vehicledevice::VehicleDeviceConfig config =
        vehicledevice::parseDeviceConfigText(text);
    CHECK(config.gps.baudrate == 115200);
}

// 32. Invalid timeout (zero) is rejected.
void test32_InvalidTimeoutRejected()
{
    const std::string text =
        "gps_enabled=false\n"
        "gps_read_timeout_ms=0\n";
    CHECK_THROWS(vehicledevice::parseDeviceConfigText(text));
}

// 32b. Invalid update interval (negative) is rejected.
void test32b_InvalidIntervalRejected()
{
    const std::string text =
        "gps_enabled=false\n"
        "gps_update_interval_ms=-5\n";
    CHECK_THROWS(vehicledevice::parseDeviceConfigText(text));
}

// 32c. Invalid simulation interval (out of range) is rejected.
void test32c_InvalidSimIntervalRejected()
{
    const std::string text =
        "gps_simulation_enabled=false\n"
        "gps_simulation_interval_ms=999999\n";
    CHECK_THROWS(vehicledevice::parseDeviceConfigText(text));
}

// 33. Duplicate keys are rejected per the project's existing config rules.
void test33_DuplicateKeyRejected()
{
    const std::string text =
        "gps_enabled=false\n"
        "gps_enabled=true\n";
    CHECK_THROWS(vehicledevice::parseDeviceConfigText(text));
}

// 33b. Unknown keys are rejected per the project's existing config rules.
void test33b_UnknownKeyRejected()
{
    const std::string text =
        "gps_enabled=false\n"
        "gps_foo=bar\n";
    CHECK_THROWS(vehicledevice::parseDeviceConfigText(text));
}

// 33c. A fully valid real-GPS config parses correctly (sanity positive).
void test33c_ValidRealConfigAccepted()
{
    const std::string text =
        "gps_enabled=true\n"
        "gps_device=/dev/ttyUSB0\n"
        "gps_baudrate=9600\n"
        "gps_data_bits=8\n"
        "gps_parity=N\n"
        "gps_stop_bits=1\n"
        "gps_read_timeout_ms=500\n"
        "gps_update_interval_ms=1000\n";
    const vehicledevice::VehicleDeviceConfig config =
        vehicledevice::parseDeviceConfigText(text);
    CHECK(config.gps.enabled);
    CHECK(config.gps.devicePath == "/dev/ttyUSB0");
    CHECK(config.gps.baudrate == 9600);
    CHECK(config.gps.readTimeoutMs == 500);
    CHECK(config.gps.updateIntervalMs == 1000);
}

// 33d. A fully valid simulation config parses correctly (sanity positive).
void test33d_ValidSimConfigAccepted()
{
    const std::string text =
        "gps_simulation_enabled=true\n"
        "gps_simulation_file=tests/gps/data/simulated_route.nmea\n"
        "gps_simulation_interval_ms=500\n"
        "gps_map_update_interval_ms=1000\n";
    const vehicledevice::VehicleDeviceConfig config =
        vehicledevice::parseDeviceConfigText(text);
    CHECK(config.gpsSimulation.enabled);
    CHECK(config.gpsSimulation.file == "tests/gps/data/simulated_route.nmea");
    CHECK(config.gpsSimulation.intervalMs == 500);
    CHECK(config.gpsSimulation.mapUpdateIntervalMs == 1000);
    CHECK(!config.gps.enabled); // real GPS auto-disabled
}

} // namespace

int main()
{
    test27_DefaultsBothFalse();
    test27b_ExplicitDisabledAccepted();
    test28_BothEnabledRejected();
    test29_RealModeEmptyDeviceRejected();
    test30_SimModeEmptyFileRejected();
    test31_InvalidBaudrateRejected();
    test31b_ValidBaudrateAccepted();
    test32_InvalidTimeoutRejected();
    test32b_InvalidIntervalRejected();
    test32c_InvalidSimIntervalRejected();
    test33_DuplicateKeyRejected();
    test33b_UnknownKeyRejected();
    test33c_ValidRealConfigAccepted();
    test33d_ValidSimConfigAccepted();

    if (g_failures == 0) {
        std::puts("gpsconfig tests passed");
        return 0;
    }
    std::printf("gpsconfig tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
