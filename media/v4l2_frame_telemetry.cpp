#include "v4l2_frame_telemetry.h"

namespace vehiclecamera {

void V4L2FrameTelemetry::reset(std::uint64_t startMonotonicNs)
{
    m_startMonotonicNs = startMonotonicNs;
    m_hasFrames = false;
    m_frames = 0;
    m_sequenceGaps = 0;
    m_firstSequence = 0;
    m_lastSequence = 0;
    m_lastFrameMonotonicNs = 0;
    m_maxFrameIntervalMs = 0.0;
}

void V4L2FrameTelemetry::observe(
    std::uint32_t sequence,
    std::uint64_t monotonicNs)
{
    if (!m_hasFrames) {
        m_hasFrames = true;
        m_firstSequence = sequence;
        m_lastFrameMonotonicNs = monotonicNs;
    } else {
        const std::uint32_t sequenceDelta =
            sequence - m_lastSequence;

        if (sequenceDelta > 1U) {
            m_sequenceGaps +=
                static_cast<std::uint64_t>(sequenceDelta - 1U);
        }

        if (monotonicNs > m_lastFrameMonotonicNs) {
            const double intervalMs =
                static_cast<double>(
                    monotonicNs - m_lastFrameMonotonicNs) /
                1000000.0;

            if (intervalMs > m_maxFrameIntervalMs)
                m_maxFrameIntervalMs = intervalMs;
        }

        m_lastFrameMonotonicNs = monotonicNs;
    }

    ++m_frames;
    m_lastSequence = sequence;
}

V4L2FrameTelemetrySnapshot V4L2FrameTelemetry::snapshot(
    std::uint64_t nowMonotonicNs) const
{
    V4L2FrameTelemetrySnapshot result;
    result.hasFrames = m_hasFrames;
    result.frames = m_frames;
    result.sequenceGaps = m_sequenceGaps;
    result.firstSequence = m_firstSequence;
    result.lastSequence = m_lastSequence;
    result.maxFrameIntervalMs = m_maxFrameIntervalMs;

    if (m_hasFrames &&
        nowMonotonicNs > m_startMonotonicNs) {
        const std::uint64_t runtimeNs =
            nowMonotonicNs - m_startMonotonicNs;

        result.fps =
            static_cast<double>(m_frames) * 1000000000.0 /
            static_cast<double>(runtimeNs);
    }

    return result;
}

} // namespace vehiclecamera
