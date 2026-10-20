// Unit tests for vehicledevice::BuzzerConfig parsing and
// buzzerAlarmToPhysicalLevel mapping.
//
// Covers:
//   - Disabled buzzer config parses successfully
//   - Enabled config with empty gpio_chip is rejected
//   - Enabled config with valid gpio_chip + gpio_line parses correctly
//   - Negative gpio_line is rejected
//   - Unknown buzzer_* key is rejected
//   - activeLow=true: active=true  -> physical 0 (beep)
//   - activeLow=true: active=false -> physical 1 (silent)
//   - activeLow=false: active=true  -> physical 1 (beep)
//   - activeLow=false: active=false -> physical 0 (silent)
//
// Build (host, no Qt):
/*   g++ -std=c++17 -Wall -Wextra -I../../common test_buzzer.cpp \
       ../../device/vehicledeviceconfig.cpp \
       ../../device/buzzerdevice.cpp \
       ../../common/logmanager.cpp -o test_buzzer  */
// Run: ./test_buzzer

#include "../../device/vehicledeviceconfig.h"
#include "../../device/buzzerdevice.h"

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

// --- BuzzerConfig parsing tests ---

void testDisabledConfigParsesSuccessfully()
{
    const std::string text =
        "buzzer_enabled=false\n"
        "buzzer_gpio_chip=\n"
        "buzzer_gpio_line=0\n"
        "buzzer_active_low=true\n";
    const vehicledevice::VehicleDeviceConfig config =
        vehicledevice::parseDeviceConfigText(text);
    CHECK(!config.buzzer.enabled);
}

void testEnabledEmptyChipIsRejected()
{
    const std::string text =
        "buzzer_enabled=true\n"
        "buzzer_gpio_chip=\n"
        "buzzer_gpio_line=3\n"
        "buzzer_active_low=true\n";
    CHECK_THROWS(vehicledevice::parseDeviceConfigText(text));
}

void testEnabledValidConfigParsesCorrectly()
{
    const std::string text =
        "buzzer_enabled=true\n"
        "buzzer_gpio_chip=/dev/gpiochip4\n"
        "buzzer_gpio_line=3\n"
        "buzzer_active_low=true\n";
    const vehicledevice::VehicleDeviceConfig config =
        vehicledevice::parseDeviceConfigText(text);
    CHECK(config.buzzer.enabled);
    CHECK(config.buzzer.gpioChip == "/dev/gpiochip4");
    CHECK(config.buzzer.gpioLine == 3);
    CHECK(config.buzzer.activeLow);
}

void testNegativeGpioLineIsRejected()
{
    const std::string text =
        "buzzer_enabled=true\n"
        "buzzer_gpio_chip=/dev/gpiochip4\n"
        "buzzer_gpio_line=-1\n"
        "buzzer_active_low=true\n";
    CHECK_THROWS(vehicledevice::parseDeviceConfigText(text));
}

void testUnknownBuzzerKeyIsRejected()
{
    const std::string text =
        "buzzer_enabled=false\n"
        "buzzer_foo=bar\n";
    CHECK_THROWS(vehicledevice::parseDeviceConfigText(text));
}

void testActiveLowFalseParsesCorrectly()
{
    const std::string text =
        "buzzer_enabled=true\n"
        "buzzer_gpio_chip=/dev/gpiochip4\n"
        "buzzer_gpio_line=3\n"
        "buzzer_active_low=false\n";
    const vehicledevice::VehicleDeviceConfig config =
        vehicledevice::parseDeviceConfigText(text);
    CHECK(config.buzzer.enabled);
    CHECK(!config.buzzer.activeLow);
}

void testGpioLineZeroIsAccepted()
{
    // gpioLine=0 is valid (GPIO line 0 exists); must not be rejected.
    const std::string text =
        "buzzer_enabled=true\n"
        "buzzer_gpio_chip=/dev/gpiochip4\n"
        "buzzer_gpio_line=0\n"
        "buzzer_active_low=true\n";
    const vehicledevice::VehicleDeviceConfig config =
        vehicledevice::parseDeviceConfigText(text);
    CHECK(config.buzzer.gpioLine == 0);
}

// --- buzzerAlarmToPhysicalLevel mapping tests ---

void testActiveLowTrueActiveMapsToZero()
{
    CHECK(vehicledevice::buzzerAlarmToPhysicalLevel(true, true) == 0);
}

void testActiveLowTrueInactiveMapsToOne()
{
    CHECK(vehicledevice::buzzerAlarmToPhysicalLevel(true, false) == 1);
}

void testActiveLowFalseActiveMapsToOne()
{
    CHECK(vehicledevice::buzzerAlarmToPhysicalLevel(false, true) == 1);
}

void testActiveLowFalseInactiveMapsToZero()
{
    CHECK(vehicledevice::buzzerAlarmToPhysicalLevel(false, false) == 0);
}

} // namespace

int main()
{
    testDisabledConfigParsesSuccessfully();
    testEnabledEmptyChipIsRejected();
    testEnabledValidConfigParsesCorrectly();
    testNegativeGpioLineIsRejected();
    testUnknownBuzzerKeyIsRejected();
    testActiveLowFalseParsesCorrectly();
    testGpioLineZeroIsAccepted();

    testActiveLowTrueActiveMapsToZero();
    testActiveLowTrueInactiveMapsToOne();
    testActiveLowFalseActiveMapsToOne();
    testActiveLowFalseInactiveMapsToZero();

    if (g_failures == 0) {
        std::puts("buzzer tests passed");
        return 0;
    }
    std::printf("buzzer tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
