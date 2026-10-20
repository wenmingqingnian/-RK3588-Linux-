#include "alarmpolicy.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

bool isDangerClass(int classId)
{
    switch (classId) {
    case 0: // person
    case 1: // bicycle
    case 2: // car
    case 3: // motorcycle
    case 5: // bus
    case 7: // truck
        return true;
    default:
        return false;
    }
}

} // namespace

namespace vehiclealarm {

TemperatureAlarmPolicy::TemperatureAlarmPolicy(double triggerCelsius,
                                               double clearCelsius)
    : triggerCelsius_(triggerCelsius)
    , clearCelsius_(clearCelsius)
{
    if (!std::isfinite(triggerCelsius_) ||
        !std::isfinite(clearCelsius_) ||
        clearCelsius_ >= triggerCelsius_) {
        throw std::invalid_argument("invalid temperature alarm thresholds");
    }
}

bool TemperatureAlarmPolicy::update(double temperatureCelsius)
{
    if (!std::isfinite(temperatureCelsius)) {
        return false;
    }

    const bool wasActive = active_;
    if (!active_ && temperatureCelsius >= triggerCelsius_) {
        active_ = true;
    } else if (active_ && temperatureCelsius <= clearCelsius_) {
        active_ = false;
    }
    return wasActive != active_;
}

bool TemperatureAlarmPolicy::isActive() const
{
    return active_;
}

DangerAlarmPolicy::DangerAlarmPolicy(double minimumArea,
                                     int triggerFrames,
                                     int clearFrames)
    : minimumArea_(minimumArea)
    , triggerFrames_(triggerFrames)
    , clearFrames_(clearFrames)
{
    if (!std::isfinite(minimumArea_) ||
        minimumArea_ <= 0.0 || minimumArea_ > 1.0 ||
        triggerFrames_ < 1 || clearFrames_ < 1) {
        throw std::invalid_argument("invalid danger alarm policy");
    }
}

bool DangerAlarmPolicy::update(
    DetectorFrameStatus status,
    const std::vector<DangerCandidate> &candidates)
{
    if (status == DetectorFrameStatus::Failed) {
        return false;
    }

    const bool wasActive = active_;
    if (frameIsDangerous(candidates)) {
        consecutiveSafeFrames_ = 0;
        consecutiveDangerousFrames_ = std::min(
            triggerFrames_, consecutiveDangerousFrames_ + 1);
        if (!active_ && consecutiveDangerousFrames_ >= triggerFrames_) {
            active_ = true;
        }
    } else {
        consecutiveDangerousFrames_ = 0;
        consecutiveSafeFrames_ = std::min(
            clearFrames_, consecutiveSafeFrames_ + 1);
        if (active_ && consecutiveSafeFrames_ >= clearFrames_) {
            active_ = false;
        }
    }

    return wasActive != active_;
}

bool DangerAlarmPolicy::reset()
{
    const bool wasActive = active_;
    active_ = false;
    consecutiveDangerousFrames_ = 0;
    consecutiveSafeFrames_ = 0;
    return wasActive;
}

bool DangerAlarmPolicy::isActive() const
{
    return active_;
}

bool DangerAlarmPolicy::frameIsDangerous(
    const std::vector<DangerCandidate> &candidates) const
{
    for (const DangerCandidate &candidate : candidates) {
        if (!isDangerClass(candidate.classId) ||
            !std::isfinite(candidate.normalizedWidth) ||
            !std::isfinite(candidate.normalizedHeight) ||
            candidate.normalizedWidth <= 0.0 ||
            candidate.normalizedHeight <= 0.0) {
            continue;
        }

        if (candidate.normalizedWidth * candidate.normalizedHeight >=
            minimumArea_) {
            return true;
        }
    }
    return false;
}

} // namespace vehiclealarm
