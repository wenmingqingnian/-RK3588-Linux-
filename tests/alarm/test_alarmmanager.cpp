#include "alarmmanager.h"
#include "alarmpolicy.h"

#include <cstdio>
#include <limits>
#include <stdexcept>
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

#define CHECK_THROWS_INVALID_ARGUMENT(expression) \
    do { \
        bool thrown = false; \
        try { \
            (void)(expression); \
        } catch (const std::invalid_argument &) { \
            thrown = true; \
        } catch (...) { \
        } \
        CHECK(thrown); \
    } while (0)

void testTemperatureAloneActivatesAggregate()
{
    vehiclealarm::AlarmManager manager;

    CHECK(manager.setSourceActive(
        vehiclealarm::AlarmSource::TemperatureHigh, true));
    CHECK(manager.isSourceActive(
        vehiclealarm::AlarmSource::TemperatureHigh));
    CHECK(manager.isAlarmActive());
}

void testDangerAloneActivatesAggregate()
{
    vehiclealarm::AlarmManager manager;

    CHECK(manager.setSourceActive(
        vehiclealarm::AlarmSource::DangerObject, true));
    CHECK(manager.isSourceActive(
        vehiclealarm::AlarmSource::DangerObject));
    CHECK(manager.isAlarmActive());
}

void testClearingOneOfTwoSourcesKeepsAggregateActive()
{
    vehiclealarm::AlarmManager manager;
    manager.setSourceActive(vehiclealarm::AlarmSource::TemperatureHigh, true);
    manager.setSourceActive(vehiclealarm::AlarmSource::DangerObject, true);

    CHECK(!manager.setSourceActive(
        vehiclealarm::AlarmSource::TemperatureHigh, false));
    CHECK(!manager.isSourceActive(
        vehiclealarm::AlarmSource::TemperatureHigh));
    CHECK(manager.isSourceActive(
        vehiclealarm::AlarmSource::DangerObject));
    CHECK(manager.isAlarmActive());
}

void testClearingLastSourceClearsAggregate()
{
    vehiclealarm::AlarmManager manager;
    manager.setSourceActive(vehiclealarm::AlarmSource::TemperatureHigh, true);
    manager.setSourceActive(vehiclealarm::AlarmSource::DangerObject, true);
    manager.setSourceActive(vehiclealarm::AlarmSource::TemperatureHigh, false);

    CHECK(manager.setSourceActive(
        vehiclealarm::AlarmSource::DangerObject, false));
    CHECK(!manager.isAlarmActive());
}

void testDuplicateStateDoesNotReportAggregateTransition()
{
    vehiclealarm::AlarmManager manager;

    CHECK(manager.setSourceActive(
        vehiclealarm::AlarmSource::TemperatureHigh, true));
    CHECK(!manager.setSourceActive(
        vehiclealarm::AlarmSource::TemperatureHigh, true));
    CHECK(manager.isAlarmActive());
}

void testClearRemovesAllSources()
{
    vehiclealarm::AlarmManager manager;
    manager.setSourceActive(vehiclealarm::AlarmSource::TemperatureHigh, true);
    manager.setSourceActive(vehiclealarm::AlarmSource::DangerObject, true);

    manager.clear();

    CHECK(!manager.isSourceActive(
        vehiclealarm::AlarmSource::TemperatureHigh));
    CHECK(!manager.isSourceActive(
        vehiclealarm::AlarmSource::DangerObject));
    CHECK(!manager.isAlarmActive());
}

vehiclealarm::DangerCandidate candidate(int classId,
                                        double width,
                                        double height)
{
    return vehiclealarm::DangerCandidate{classId, width, height};
}

void testTemperatureHysteresisBoundaries()
{
    vehiclealarm::TemperatureAlarmPolicy policy;

    CHECK(policy.update(40.0));
    CHECK(policy.isActive());
    CHECK(!policy.update(39.0));
    CHECK(policy.isActive());
    CHECK(policy.update(38.0));
    CHECK(!policy.isActive());
}

void testTemperatureInsideBandPreservesInactiveState()
{
    vehiclealarm::TemperatureAlarmPolicy policy;

    CHECK(!policy.update(39.0));
    CHECK(!policy.isActive());
}

void testTemperatureNonFiniteValuesPreserveState()
{
    vehiclealarm::TemperatureAlarmPolicy policy;
    policy.update(41.0);

    CHECK(!policy.update(std::numeric_limits<double>::quiet_NaN()));
    CHECK(policy.isActive());
    CHECK(!policy.update(std::numeric_limits<double>::infinity()));
    CHECK(policy.isActive());
}

void testInvalidTemperatureThresholdsAreRejected()
{
    CHECK_THROWS_INVALID_ARGUMENT(
        vehiclealarm::TemperatureAlarmPolicy(40.0, 40.0));
    CHECK_THROWS_INVALID_ARGUMENT(
        vehiclealarm::TemperatureAlarmPolicy(38.0, 40.0));
}

void testListedClassAtMinimumAreaIsDangerous()
{
    vehiclealarm::DangerAlarmPolicy policy(0.12, 1, 1);
    const std::vector<vehiclealarm::DangerCandidate> candidates{
        candidate(0, 0.12, 1.0)
    };

    CHECK(policy.update(vehiclealarm::DetectorFrameStatus::Succeeded,
                        candidates));
    CHECK(policy.isActive());
}

void testUnlistedAndInvalidCandidatesAreSafe()
{
    vehiclealarm::DangerAlarmPolicy policy(0.12, 1, 1);

    CHECK(!policy.update(vehiclealarm::DetectorFrameStatus::Succeeded,
                         {candidate(4, 1.0, 1.0)}));
    CHECK(!policy.update(vehiclealarm::DetectorFrameStatus::Succeeded,
                         {candidate(0, -1.0, 1.0)}));
    CHECK(!policy.update(
        vehiclealarm::DetectorFrameStatus::Succeeded,
        {candidate(0, std::numeric_limits<double>::quiet_NaN(), 1.0)}));
    CHECK(!policy.isActive());
}

void testThreeConsecutiveDangerousFramesActivate()
{
    vehiclealarm::DangerAlarmPolicy policy;
    const std::vector<vehiclealarm::DangerCandidate> candidates{
        candidate(2, 0.5, 0.5)
    };

    CHECK(!policy.update(vehiclealarm::DetectorFrameStatus::Succeeded,
                         candidates));
    CHECK(!policy.update(vehiclealarm::DetectorFrameStatus::Succeeded,
                         candidates));
    CHECK(!policy.isActive());
    CHECK(policy.update(vehiclealarm::DetectorFrameStatus::Succeeded,
                        candidates));
    CHECK(policy.isActive());
}

void testSafeFrameResetsDangerousCounter()
{
    vehiclealarm::DangerAlarmPolicy policy;
    const std::vector<vehiclealarm::DangerCandidate> candidates{
        candidate(5, 0.5, 0.5)
    };

    policy.update(vehiclealarm::DetectorFrameStatus::Succeeded, candidates);
    policy.update(vehiclealarm::DetectorFrameStatus::Succeeded, candidates);
    policy.update(vehiclealarm::DetectorFrameStatus::Succeeded, {});
    policy.update(vehiclealarm::DetectorFrameStatus::Succeeded, candidates);
    policy.update(vehiclealarm::DetectorFrameStatus::Succeeded, candidates);

    CHECK(!policy.isActive());
}

void testTenConsecutiveSafeFramesClear()
{
    vehiclealarm::DangerAlarmPolicy policy;
    const std::vector<vehiclealarm::DangerCandidate> candidates{
        candidate(7, 0.5, 0.5)
    };
    for (int i = 0; i < 3; ++i) {
        policy.update(vehiclealarm::DetectorFrameStatus::Succeeded,
                      candidates);
    }

    for (int i = 0; i < 9; ++i) {
        CHECK(!policy.update(vehiclealarm::DetectorFrameStatus::Succeeded,
                             {}));
        CHECK(policy.isActive());
    }

    CHECK(policy.update(vehiclealarm::DetectorFrameStatus::Succeeded, {}));
    CHECK(!policy.isActive());
}

void testDetectorFailurePreservesStateAndCounters()
{
    vehiclealarm::DangerAlarmPolicy policy;
    const std::vector<vehiclealarm::DangerCandidate> candidates{
        candidate(1, 0.5, 0.5)
    };

    policy.update(vehiclealarm::DetectorFrameStatus::Succeeded, candidates);
    policy.update(vehiclealarm::DetectorFrameStatus::Succeeded, candidates);
    CHECK(!policy.update(vehiclealarm::DetectorFrameStatus::Failed, {}));
    CHECK(policy.update(vehiclealarm::DetectorFrameStatus::Succeeded,
                        candidates));
    CHECK(policy.isActive());
}

void testDangerResetClearsActiveStateAndCounters()
{
    vehiclealarm::DangerAlarmPolicy policy(0.12, 1, 1);
    policy.update(vehiclealarm::DetectorFrameStatus::Succeeded,
                  {candidate(3, 0.5, 0.5)});

    CHECK(policy.reset());
    CHECK(!policy.isActive());
    CHECK(!policy.reset());
}

void testInvalidDangerConfigurationIsRejected()
{
    CHECK_THROWS_INVALID_ARGUMENT(
        vehiclealarm::DangerAlarmPolicy(0.0, 3, 10));
    CHECK_THROWS_INVALID_ARGUMENT(
        vehiclealarm::DangerAlarmPolicy(1.1, 3, 10));
    CHECK_THROWS_INVALID_ARGUMENT(
        vehiclealarm::DangerAlarmPolicy(0.12, 0, 10));
    CHECK_THROWS_INVALID_ARGUMENT(
        vehiclealarm::DangerAlarmPolicy(0.12, 3, 0));
}

} // namespace

int main()
{
    testTemperatureAloneActivatesAggregate();
    testDangerAloneActivatesAggregate();
    testClearingOneOfTwoSourcesKeepsAggregateActive();
    testClearingLastSourceClearsAggregate();
    testDuplicateStateDoesNotReportAggregateTransition();
    testClearRemovesAllSources();

    testTemperatureHysteresisBoundaries();
    testTemperatureInsideBandPreservesInactiveState();
    testTemperatureNonFiniteValuesPreserveState();
    testInvalidTemperatureThresholdsAreRejected();
    testListedClassAtMinimumAreaIsDangerous();
    testUnlistedAndInvalidCandidatesAreSafe();
    testThreeConsecutiveDangerousFramesActivate();
    testSafeFrameResetsDangerousCounter();
    testTenConsecutiveSafeFramesClear();
    testDetectorFailurePreservesStateAndCounters();
    testDangerResetClearsActiveStateAndCounters();
    testInvalidDangerConfigurationIsRejected();

    if (g_failures == 0) {
        std::puts("alarm manager and policy tests passed");
        return 0;
    }

    std::printf("alarm manager tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
