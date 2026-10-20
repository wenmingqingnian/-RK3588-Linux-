#ifndef V4L2_FRAME_TELEMETRY_H
#define V4L2_FRAME_TELEMETRY_H

#include <cstdint>

namespace vehiclecamera {

struct V4L2FrameTelemetrySnapshot {
    bool hasFrames = false;
    std::uint64_t frames = 0;
    std::uint64_t sequenceGaps = 0;
    std::uint32_t firstSequence = 0;
    std::uint32_t lastSequence = 0;
    double fps = 0.0;
    double maxFrameIntervalMs = 0.0;
};

class V4L2FrameTelemetry
{
public:
    void reset(std::uint64_t startMonotonicNs);
    void observe(std::uint32_t sequence, std::uint64_t monotonicNs);
    V4L2FrameTelemetrySnapshot snapshot(
        std::uint64_t nowMonotonicNs) const;

private:
    std::uint64_t m_startMonotonicNs = 0;
    bool m_hasFrames = false;
    std::uint64_t m_frames = 0;
    std::uint64_t m_sequenceGaps = 0;
    std::uint32_t m_firstSequence = 0;
    std::uint32_t m_lastSequence = 0;
    std::uint64_t m_lastFrameMonotonicNs = 0;
    double m_maxFrameIntervalMs = 0.0;
};

} // namespace vehiclecamera

#endif // V4L2_FRAME_TELEMETRY_H
