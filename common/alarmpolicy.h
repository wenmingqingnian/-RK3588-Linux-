#ifndef ALARMPOLICY_H
#define ALARMPOLICY_H

#include <vector>

namespace vehiclealarm {

class TemperatureAlarmPolicy {
public:
    TemperatureAlarmPolicy(double triggerCelsius = 40.0,
                           double clearCelsius = 38.0);

    bool update(double temperatureCelsius);
    bool isActive() const;

private:
    double triggerCelsius_;
    double clearCelsius_;
    bool active_ = false;
};

struct DangerCandidate {
    int classId;
    double normalizedWidth;
    double normalizedHeight;
};

enum class DetectorFrameStatus {
    Succeeded,
    Failed
};

class DangerAlarmPolicy {
public:
    // triggerFrames/clearFrames are tuned for ~6 Hz RKNN detection:
    //   trigger=15  → ~2.5s of continuous dangerous detections before alarm
    //   clear=60    → ~10s of continuous safe frames before clear
    // This prevents buzzer chatter when detection flickers due to partial
    // occlusion or brief mis-detections (typical YOLO behavior).
    DangerAlarmPolicy(double minimumArea = 0.12,
                      int triggerFrames = 15,
                      int clearFrames = 60);

    bool update(DetectorFrameStatus status,
                const std::vector<DangerCandidate> &candidates);
    bool reset();
    bool isActive() const;

private:
    bool frameIsDangerous(
        const std::vector<DangerCandidate> &candidates) const;

    double minimumArea_;
    int triggerFrames_;
    int clearFrames_;
    int consecutiveDangerousFrames_ = 0;
    int consecutiveSafeFrames_ = 0;
    bool active_ = false;
};

} // namespace vehiclealarm

#endif // ALARMPOLICY_H
