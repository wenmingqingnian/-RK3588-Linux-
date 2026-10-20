#ifndef ALARMMANAGER_H
#define ALARMMANAGER_H

#include <array>

namespace vehiclealarm {

enum class AlarmSource {
    TemperatureHigh,
    DangerObject
};

class AlarmManager {
public:
    bool setSourceActive(AlarmSource source, bool active);
    bool isSourceActive(AlarmSource source) const;
    bool isAlarmActive() const;
    void clear();

private:
    std::array<bool, 2> sourceStates_{{false, false}};
};

} // namespace vehiclealarm

#endif // ALARMMANAGER_H
