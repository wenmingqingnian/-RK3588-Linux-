#ifndef MEDIA_MEDIA_PIPELINE_H
#define MEDIA_MEDIA_PIPELINE_H

#include "frame_broker.h"
#include "frame_source.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace media {

// Frame consumer callback. Invoked on the consumer's own thread.
// Must be thread-safe and non-blocking (slow consumers cause frame drops
// via the broker's drop_oldest policy, not pipeline stalls).
using FrameConsumer = std::function<void(const FramePacket &)>;

struct MediaConsumerStatistics {
    uint64_t processedFrames = 0;
    uint64_t droppedFrames = 0;
    int queueDepth = 0;
    int maxQueueDepth = 0;
    double fps = 0.0;
    double averageProcessingMs = 0.0;
    double p95ProcessingMs = 0.0;
};

struct MediaPipelineStatistics {
    int queueCapacity = 0;
    uint64_t sourceFrames = 0;
    double runtimeSeconds = 0.0;
    double sourceFps = 0.0;
    MediaConsumerStatistics gui;
    MediaConsumerStatistics rknn;
    MediaConsumerStatistics rtsp;
};

// MediaPipeline wires a single IFrameSource to three independent consumer
// lanes (GUI, RKNN, RTSP) via a FrameBroker. Each lane has its own bounded
// queue, so a slow or disabled lane does not block the others.
//
// Lifecycle: setSource/setConsumer* → start → stop → (setSource again) → start
//
// start() opens the source synchronously and only returns true after the
// source has been successfully opened and all threads have been launched.
// stop() is idempotent and blocks until all threads have exited.
class MediaPipeline {
public:
    enum class State { Idle, Starting, Running, Stopping, Failed };

    explicit MediaPipeline(int queueCapacity = 3,
                           int sourceReadTimeoutMs = 500,
                           int consumerTakeTimeoutMs = 200);
    ~MediaPipeline();

    MediaPipeline(const MediaPipeline &) = delete;
    MediaPipeline &operator=(const MediaPipeline &) = delete;

    // Take ownership of the source. Must be called before start().
    void setSource(std::unique_ptr<IFrameSource> source);

    // Set consumer callbacks. A null std::function disables that lane.
    void setGuiConsumer(FrameConsumer consumer);
    void setRknnConsumer(FrameConsumer consumer);
    void setRtspConsumer(FrameConsumer consumer);

    // Runtime lane gating is safe while the pipeline is running. The worker
    // thread remains alive, but disabled lanes stop accepting/buffering
    // frames and any queued frames are released immediately.
    void setLaneEnabled(ConsumerLane lane, bool enabled);

    // Start the pipeline. Returns true only if the source opened successfully
    // and all threads launched. On failure, state transitions to Failed and
    // an error message is written (if provided).
    bool start(std::string *errorMessage);

    // Stop the pipeline. Idempotent. Blocks until all threads exit.
    void stop();

    State state() const { return m_state.load(std::memory_order_acquire); }

    // Statistics.
    int droppedCount(ConsumerLane lane) const { return m_broker.droppedCount(lane); }
    int queueDepth(ConsumerLane lane) const { return m_broker.queueDepth(lane); }
    int queueCapacity() const { return m_broker.capacity(); }
    MediaPipelineStatistics statistics() const;

private:
    struct LaneTiming {
        std::atomic<uint64_t> processedFrames{0};
        std::atomic<uint64_t> totalProcessingUs{0};
        mutable std::mutex samplesMutex;
        std::deque<uint64_t> recentProcessingUs;
    };

    void sourceLoop();
    void consumerLoop(ConsumerLane lane, FrameConsumer consumer);
    void resetStatistics();
    void recordProcessingTime(ConsumerLane lane, uint64_t elapsedUs);
    MediaConsumerStatistics consumerStatistics(ConsumerLane lane,
                                               double runtimeSeconds) const;

    std::unique_ptr<IFrameSource> m_source;
    FrameBroker m_broker;

    std::atomic<State> m_state{State::Idle};
    std::atomic<bool> m_stopRequested{false};

    int m_sourceReadTimeoutMs;
    int m_consumerTakeTimeoutMs;

    std::atomic<uint64_t> m_sourceFrames{0};
    std::atomic<uint64_t> m_startedAtUs{0};
    std::atomic<uint64_t> m_stoppedAtUs{0};
    std::array<LaneTiming, static_cast<int>(ConsumerLane::kCount)> m_laneTiming;

    std::thread m_sourceThread;
    std::thread m_guiThread;
    std::thread m_rknnThread;
    std::thread m_rtspThread;

    FrameConsumer m_guiConsumer;
    FrameConsumer m_rknnConsumer;
    FrameConsumer m_rtspConsumer;
};

} // namespace media

#endif // MEDIA_MEDIA_PIPELINE_H
