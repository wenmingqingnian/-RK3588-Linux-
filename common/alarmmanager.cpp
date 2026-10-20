#include "alarmmanager.h"

#include <cstddef>
#include <stdexcept>

namespace {

std::size_t sourceIndex(vehiclealarm::AlarmSource source)
{
    switch (source) {
    case vehiclealarm::AlarmSource::TemperatureHigh:
        return 0;
    case vehiclealarm::AlarmSource::DangerObject:
        return 1;
    }

    throw std::invalid_argument("unknown alarm source");
}

} // namespace

namespace vehiclealarm {

bool AlarmManager::setSourceActive(AlarmSource source, bool active)
{
    const bool wasActive = isAlarmActive();
    sourceStates_[sourceIndex(source)] = active;
    return wasActive != isAlarmActive();
}

bool AlarmManager::isSourceActive(AlarmSource source) const
{
    return sourceStates_[sourceIndex(source)];
}

bool AlarmManager::isAlarmActive() const
{
    return sourceStates_[0] || sourceStates_[1];
}

void AlarmManager::clear()
{
    sourceStates_.fill(false);
}

} // namespace vehiclealarm
