#include "media_pipeline.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace media {

namespace {

constexpr size_t kRecentTimingSampleLimit = 256;

uint64_t steadyNowUs()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

MediaPipeline::MediaPipeline(int queueCapacity,
                             int sourceReadTimeoutMs,
                             int consumerTakeTimeoutMs)
    : m_broker(queueCapacity)
    , m_sourceReadTimeoutMs(sourceReadTimeoutMs > 0 ? sourceReadTimeoutMs : 500)
    , m_consumerTakeTimeoutMs(consumerTakeTimeoutMs > 0
                                  ? consumerTakeTimeoutMs
                                  : 200)
{
}

MediaPipeline::~MediaPipeline()
{
    stop();
}

void MediaPipeline::setSource(std::unique_ptr<IFrameSource> source)
{
    // If a previous source is still attached, stop and close it first.
    if (m_source) {
        m_source->requestStop();
        m_source->close();
        m_source.reset();
    }
    m_source = std::move(source);
}

void MediaPipeline::setGuiConsumer(FrameConsumer consumer)
{
    m_guiConsumer = std::move(consumer);
    m_broker.setLaneEnabled(ConsumerLane::Gui,
                            static_cast<bool>(m_guiConsumer));
}

void MediaPipeline::setRknnConsumer(FrameConsumer consumer)
{
    m_rknnConsumer = std::move(consumer);
    m_broker.setLaneEnabled(ConsumerLane::Rknn,
                            static_cast<bool>(m_rknnConsumer));
}

void MediaPipeline::setRtspConsumer(FrameConsumer consumer)
{
    m_rtspConsumer = std::move(consumer);
    m_broker.setLaneEnabled(ConsumerLane::Rtsp,
                            static_cast<bool>(m_rtspConsumer));
}

void MediaPipeline::setLaneEnabled(ConsumerLane lane, bool enabled)
{
    m_broker.setLaneEnabled(lane, enabled);
}

bool MediaPipeline::start(std::string *errorMessage)
{
    if (m_state.load(std::memory_order_acquire) == State::Running) {
        if (errorMessage) *errorMessage = "pipeline already running";
        return false;
    }

    m_state.store(State::Starting, std::memory_order_release);

    if (!m_source) {
        if (errorMessage) *errorMessage = "no source configured";
        m_state.store(State::Failed, std::memory_order_release);
        return false;
    }

    // Open the source synchronously — start() must only report success after
    // the source is actually ready to produce frames.
    std::string openError;
    if (!m_source->open(&openError)) {
        if (errorMessage)
            *errorMessage = "source open failed: " + openError;
        m_state.store(State::Failed, std::memory_order_release);
        return false;
    }

    // Reset broker for a fresh run (clears stop flag and queues).
    m_broker.reset();
    resetStatistics();
    m_stopRequested.store(false, std::memory_order_release);
    m_startedAtUs.store(steadyNowUs(), std::memory_order_release);

    // Launch source thread.
    m_sourceThread = std::thread([this]() { this->sourceLoop(); });

    // Launch consumer threads only for enabled lanes.
    if (m_guiConsumer)
        m_guiThread = std::thread([this]() {
            this->consumerLoop(ConsumerLane::Gui, m_guiConsumer);
        });
    if (m_rknnConsumer)
        m_rknnThread = std::thread([this]() {
            this->consumerLoop(ConsumerLane::Rknn, m_rknnConsumer);
        });
    if (m_rtspConsumer)
        m_rtspThread = std::thread([this]() {
            this->consumerLoop(ConsumerLane::Rtsp, m_rtspConsumer);
        });

    m_state.store(State::Running, std::memory_order_release);
    return true;
}

void MediaPipeline::stop()
{
    State expected = State::Running;
    if (!m_state.compare_exchange_strong(expected, State::Stopping)) {
        // Not running; could be Idle, Starting, Stopping, or Failed.
        // In all cases, ensure threads are joined if any linger.
        if (m_state.load() == State::Idle)
            return;
    }

    m_stopRequested.store(true, std::memory_order_release);

    // 1. Wake the source (unblocks any blocking read).
    if (m_source)
        m_source->requestStop();

    // 2. Wake all consumers (unblocks broker.take).
    m_broker.requestStop();

    // 3. Join threads in order: source first, then consumers.
    if (m_sourceThread.joinable())
        m_sourceThread.join();
    if (m_guiThread.joinable())
        m_guiThread.join();
    if (m_rknnThread.joinable())
        m_rknnThread.join();
    if (m_rtspThread.joinable())
        m_rtspThread.join();

    // 4. Close the source.
    if (m_source)
        m_source->close();

    m_stoppedAtUs.store(steadyNowUs(), std::memory_order_release);
    m_state.store(State::Idle, std::memory_order_release);
}

void MediaPipeline::sourceLoop()
{
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        FramePacket frame;
        std::string error;
        const FrameReadResult result =
            m_source->readFrame(&frame, m_sourceReadTimeoutMs, &error);

        if (result == FrameReadResult::FrameReady) {
            m_sourceFrames.fetch_add(1, std::memory_order_relaxed);
            m_broker.publish(frame);
        } else if (result == FrameReadResult::EndOfStream) {
            // Source exhausted (file with no loop). Exit the source thread;
            // consumers remain blocked on their queues until stop() is called
            // or the pipeline is restarted.
            break;
        } else if (result == FrameReadResult::Stopped) {
            break;
        } else if (result == FrameReadResult::Timeout) {
            // Transient; retry.
            continue;
        } else {
            // RecoverableError or FatalError: exit the source thread.
            break;
        }
    }
}

void MediaPipeline::consumerLoop(ConsumerLane lane, FrameConsumer consumer)
{
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        FramePacket frame;
        const FrameReadResult result =
            m_broker.take(lane, &frame, m_consumerTakeTimeoutMs);

        if (result == FrameReadResult::FrameReady) {
            if (consumer) {
                const uint64_t startedUs = steadyNowUs();
                consumer(frame);
                recordProcessingTime(lane, steadyNowUs() - startedUs);
            }
        } else if (result == FrameReadResult::Stopped) {
            break;
        } else if (result == FrameReadResult::Timeout) {
            // Check stop flag and retry.
            continue;
        } else {
            // EndOfStream / errors: shouldn't come from broker; exit.
            break;
        }
    }
}

void MediaPipeline::resetStatistics()
{
    m_sourceFrames.store(0, std::memory_order_relaxed);
    m_startedAtUs.store(0, std::memory_order_relaxed);
    m_stoppedAtUs.store(0, std::memory_order_relaxed);
    for (LaneTiming &timing : m_laneTiming) {
        timing.processedFrames.store(0, std::memory_order_relaxed);
        timing.totalProcessingUs.store(0, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(timing.samplesMutex);
        timing.recentProcessingUs.clear();
    }
}

void MediaPipeline::recordProcessingTime(ConsumerLane lane, uint64_t elapsedUs)
{
    LaneTiming &timing = m_laneTiming[static_cast<int>(lane)];
    timing.processedFrames.fetch_add(1, std::memory_order_relaxed);
    timing.totalProcessingUs.fetch_add(elapsedUs, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(timing.samplesMutex);
    if (timing.recentProcessingUs.size() >= kRecentTimingSampleLimit)
        timing.recentProcessingUs.pop_front();
    timing.recentProcessingUs.push_back(elapsedUs);
}

MediaConsumerStatistics MediaPipeline::consumerStatistics(
    ConsumerLane lane, double runtimeSeconds) const
{
    MediaConsumerStatistics result;
    const LaneTiming &timing = m_laneTiming[static_cast<int>(lane)];
    result.processedFrames =
        timing.processedFrames.load(std::memory_order_relaxed);
    result.droppedFrames = static_cast<uint64_t>(m_broker.droppedCount(lane));
    result.queueDepth = m_broker.queueDepth(lane);
    result.maxQueueDepth = m_broker.maxQueueDepth(lane);
    if (runtimeSeconds > 0.0) {
        result.fps =
            static_cast<double>(result.processedFrames) / runtimeSeconds;
    }
    if (result.processedFrames > 0) {
        result.averageProcessingMs =
            static_cast<double>(timing.totalProcessingUs.load(
                std::memory_order_relaxed)) /
            static_cast<double>(result.processedFrames) / 1000.0;
    }

    std::vector<uint64_t> samples;
    {
        std::lock_guard<std::mutex> lock(timing.samplesMutex);
        samples.assign(timing.recentProcessingUs.begin(),
                       timing.recentProcessingUs.end());
    }
    if (!samples.empty()) {
        std::sort(samples.begin(), samples.end());
        const size_t p95Index = ((samples.size() * 95 + 99) / 100) - 1;
        result.p95ProcessingMs =
            static_cast<double>(samples[p95Index]) / 1000.0;
    }
    return result;
}

MediaPipelineStatistics MediaPipeline::statistics() const
{
    MediaPipelineStatistics result;
    result.queueCapacity = m_broker.capacity();
    result.sourceFrames = m_sourceFrames.load(std::memory_order_relaxed);

    const uint64_t startedAtUs = m_startedAtUs.load(std::memory_order_acquire);
    uint64_t stoppedAtUs = m_stoppedAtUs.load(std::memory_order_acquire);
    if (startedAtUs > 0) {
        if (stoppedAtUs == 0)
            stoppedAtUs = steadyNowUs();
        if (stoppedAtUs >= startedAtUs) {
            result.runtimeSeconds =
                static_cast<double>(stoppedAtUs - startedAtUs) / 1000000.0;
        }
    }
    if (result.runtimeSeconds > 0.0) {
        result.sourceFps =
            static_cast<double>(result.sourceFrames) / result.runtimeSeconds;
    }

    result.gui = consumerStatistics(ConsumerLane::Gui, result.runtimeSeconds);
    result.rknn = consumerStatistics(ConsumerLane::Rknn, result.runtimeSeconds);
    result.rtsp = consumerStatistics(ConsumerLane::Rtsp, result.runtimeSeconds);
    return result;
}

} // namespace media
