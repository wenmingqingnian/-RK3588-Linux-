// QuarkPi-CA2 multi-source alarm board diagnostic.
//
// Build from the project root with the command documented in
// docs/BUILD_RUN_TEST.md under "Verify Multi-Source Alarm Integration".
//
// Run as the normal quark user after installing the gpio udev rule:
//   ./tests/alarm/test_alarm_board

#include "alarmmanager.h"
#include "alarmpolicy.h"
#include "buzzerdevice.h"

#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::printf("FAIL [%s:%d]: %s\n", \
                        __FILE__, __LINE__, #condition); \
            ++g_failures; \
        } \
    } while (0)

bool setBuzzer(vehicledevice::BuzzerDevice &buzzer, bool active)
{
    std::string error;
    if (!buzzer.setAlarm(active, &error)) {
        std::fprintf(stderr, "setAlarm(%d) failed: %s\n",
                     static_cast<int>(active), error.c_str());
        ++g_failures;
        return false;
    }
    return true;
}

void pulseBuzzer(vehicledevice::BuzzerDevice &buzzer)
{
    std::puts("aggregate alarm active: beep for 200 ms");
    if (!setBuzzer(buzzer, true)) {
        return;
    }
    ::usleep(200000);
    setBuzzer(buzzer, false);
}

} // namespace

int main()
{
    vehicledevice::BuzzerConfig config;
    config.enabled = true;
    config.gpioChip = "/dev/gpiochip4";
    config.gpioLine = 3;
    config.activeLow = true;

    vehicledevice::BuzzerDevice buzzer(config);
    std::string error;
    if (!buzzer.openDevice(&error)) {
        std::fprintf(stderr, "open failed: %s\n", error.c_str());
        return 1;
    }

    vehiclealarm::AlarmManager manager;
    vehiclealarm::TemperatureAlarmPolicy temperaturePolicy;
    vehiclealarm::DangerAlarmPolicy dangerPolicy;
    const std::vector<vehiclealarm::DangerCandidate> dangerCandidates{
        vehiclealarm::DangerCandidate{0, 0.5, 0.5}
    };

    std::puts("simulate temperature 41 C");
    CHECK(temperaturePolicy.update(41.0));
    CHECK(manager.setSourceActive(
        vehiclealarm::AlarmSource::TemperatureHigh,
        temperaturePolicy.isActive()));
    CHECK(manager.isAlarmActive());
    pulseBuzzer(buzzer);

    std::puts("simulate person danger for 3 consecutive frames");
    for (int i = 0; i < 3; ++i) {
        const bool dangerChanged = dangerPolicy.update(
            vehiclealarm::DetectorFrameStatus::Succeeded,
            dangerCandidates);
        if (dangerChanged) {
            CHECK(!manager.setSourceActive(
                vehiclealarm::AlarmSource::DangerObject,
                dangerPolicy.isActive()));
        }
    }
    CHECK(dangerPolicy.isActive());
    CHECK(manager.isAlarmActive());

    std::puts("simulate temperature 37 C while danger remains active");
    CHECK(temperaturePolicy.update(37.0));
    CHECK(!manager.setSourceActive(
        vehiclealarm::AlarmSource::TemperatureHigh,
        temperaturePolicy.isActive()));
    CHECK(!manager.isSourceActive(
        vehiclealarm::AlarmSource::TemperatureHigh));
    CHECK(manager.isSourceActive(vehiclealarm::AlarmSource::DangerObject));
    CHECK(manager.isAlarmActive());

    std::puts("simulate 10 consecutive safe frames");
    for (int i = 0; i < 10; ++i) {
        const bool dangerChanged = dangerPolicy.update(
            vehiclealarm::DetectorFrameStatus::Succeeded, {});
        if (dangerChanged) {
            CHECK(manager.setSourceActive(
                vehiclealarm::AlarmSource::DangerObject,
                dangerPolicy.isActive()));
        }
    }
    CHECK(!dangerPolicy.isActive());
    CHECK(!manager.isAlarmActive());
    CHECK(setBuzzer(buzzer, false));

    std::puts("verify explicit exit silence");
    manager.clear();
    CHECK(setBuzzer(buzzer, false));

    if (g_failures == 0) {
        std::puts("alarm board diagnostic passed");
        return 0;
    }

    std::printf("alarm board diagnostic FAILED: %d failure(s)\n", g_failures);
    return 2;
}
