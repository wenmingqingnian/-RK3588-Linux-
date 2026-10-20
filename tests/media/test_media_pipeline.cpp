// Unit tests for MediaPipeline (source + broker + 3 consumer threads).
// Qt-free: compiles with plain g++ under -std=c++17.
#include "media_pipeline.h"
#include "test_pattern_source.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

namespace {

int g_failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// A minimal source that produces N frames then returns EndOfStream.
// Used to drive the pipeline without relying on timing. An optional
// inter-frame delay (microseconds) lets consumers keep up so tests can
// verify exact frame counts without drop_oldest losses.
class LimitedSource : public media::IFrameSource {
public:
    LimitedSource(int frameCount, int width = 160, int height = 120,
                  int delayUs = 1000)
        : m_frameCount(frameCount), m_width(width), m_height(height),
          m_delayUs(delayUs) {}

    bool open(std::string *errorMessage) override {
        (void)errorMessage;
        m_opened = true;
        return true;
    }
    media::FrameReadResult readFrame(media::FramePacket *frame,
                                     int timeoutMs,
                                     std::string *errorMessage) override {
        (void)timeoutMs;
        (void)errorMessage;
        if (!m_opened) return media::FrameReadResult::FatalError;
        if (m_produced >= m_frameCount) return media::FrameReadResult::EndOfStream;
        if (m_stopRequested) return media::FrameReadResult::Stopped;

        if (m_delayUs > 0)
            std::this_thread::sleep_for(std::chrono::microseconds(m_delayUs));

        auto buf = std::make_shared<std::vector<uint8_t>>(4);
        (*buf)[0] = static_cast<uint8_t>(m_produced & 0xff);
        frame->data = buf;
        frame->width = m_width;
        frame->height = m_height;
        frame->format = media::PixelFormat::NV12;
        frame->sequence = static_cast<uint64_t>(m_produced);
        frame->timestampUs = static_cast<uint64_t>(m_produced) * 1000;
        ++m_produced;
        return media::FrameReadResult::FrameReady;
    }
    void requestStop() override { m_stopRequested = true; }
    void close() override { requestStop(); m_opened = false; }

private:
    int m_frameCount;
    int m_width;
    int m_height;
    int m_delayUs;
    int m_produced = 0;
    bool m_opened = false;
    std::atomic<bool> m_stopRequested{false};
};

void testStartFailsWithoutSource()
{
    media::MediaPipeline pipeline;
    std::string error;
    CHECK(!pipeline.start(&error));
    CHECK(!error.empty());
    CHECK(pipeline.state() == media::MediaPipeline::State::Failed);
}

void testConfiguredQueueCapacityIsApplied()
{
    media::MediaPipeline pipeline(1);
    CHECK(pipeline.queueCapacity() == 1);
}

void testStartSucceedsWithSourceAndOpen()
{
    media::MediaPipeline pipeline;
    pipeline.setSource(std::make_unique<media::TestPatternSource>(160, 120, 30));
    std::string error;
    CHECK(pipeline.start(&error));
    CHECK(pipeline.state() == media::MediaPipeline::State::Running);
    pipeline.stop();
    CHECK(pipeline.state() == media::MediaPipeline::State::Idle);
}

void testConsumersReceiveFrames()
{
    media::MediaPipeline pipeline;
    pipeline.setSource(std::make_unique<LimitedSource>(5));

    std::atomic<int> guiCount{0};
    std::atomic<int> rknnCount{0};
    std::atomic<int> rtspCount{0};

    pipeline.setGuiConsumer([&guiCount](const media::FramePacket &) { guiCount.fetch_add(1); });
    pipeline.setRknnConsumer([&rknnCount](const media::FramePacket &) { rknnCount.fetch_add(1); });
    pipeline.setRtspConsumer([&rtspCount](const media::FramePacket &) { rtspCount.fetch_add(1); });

    std::string error;
    CHECK(pipeline.start(&error));
    // Wait for source to produce 5 frames and consumers to process them.
    for (int i = 0; i < 100 && guiCount.load() < 5; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    pipeline.stop();

    CHECK(guiCount.load() == 5);
    CHECK(rknnCount.load() == 5);
    CHECK(rtspCount.load() == 5);
}

void testStopUnblocksAllThreads()
{
    media::MediaPipeline pipeline;
    pipeline.setSource(std::make_unique<media::TestPatternSource>(160, 120, 30));

    std::atomic<int> guiCount{0};
    pipeline.setGuiConsumer([&guiCount](const media::FramePacket &) { guiCount.fetch_add(1); });

    std::string error;
    CHECK(pipeline.start(&error));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pipeline.stop();
    // After stop returns, all threads have exited.
    CHECK(pipeline.state() == media::MediaPipeline::State::Idle);
    CHECK(guiCount.load() > 0);
}

void testRepeatedStartStop()
{
    media::MediaPipeline pipeline;
    for (int i = 0; i < 20; ++i) {
        pipeline.setSource(std::make_unique<LimitedSource>(2));
        std::atomic<int> count{0};
        pipeline.setGuiConsumer([&count](const media::FramePacket &) { count.fetch_add(1); });
        pipeline.setRknnConsumer([](const media::FramePacket &) {});
        pipeline.setRtspConsumer([](const media::FramePacket &) {});

        std::string error;
        CHECK(pipeline.start(&error));
        for (int w = 0; w < 100 && count.load() < 2; ++w)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pipeline.stop();
        CHECK(count.load() == 2);
    }
}

void testStopIsIdempotent()
{
    media::MediaPipeline pipeline;
    pipeline.setSource(std::make_unique<media::TestPatternSource>(160, 120, 30));
    std::string error;
    pipeline.start(&error);
    pipeline.stop();
    pipeline.stop();
    pipeline.stop();
    CHECK(pipeline.state() == media::MediaPipeline::State::Idle);
}

void testDisabledLaneDoesNotBlockOthers()
{
    media::MediaPipeline pipeline;
    pipeline.setSource(std::make_unique<LimitedSource>(5));

    std::atomic<int> guiCount{0};
    std::atomic<int> rtspCount{0};
    // RKNN lane disabled (no consumer set).
    pipeline.setGuiConsumer([&guiCount](const media::FramePacket &) { guiCount.fetch_add(1); });
    pipeline.setRtspConsumer([&rtspCount](const media::FramePacket &) { rtspCount.fetch_add(1); });

    std::string error;
    CHECK(pipeline.start(&error));
    for (int i = 0; i < 100 && guiCount.load() < 5; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    pipeline.stop();

    CHECK(guiCount.load() == 5);
    CHECK(rtspCount.load() == 5);
}

void testDestructorJoinsThreads()
{
    std::atomic<int> guiCount{0};
    {
        media::MediaPipeline pipeline;
        pipeline.setSource(std::make_unique<media::TestPatternSource>(160, 120, 30));
        pipeline.setGuiConsumer([&guiCount](const media::FramePacket &) { guiCount.fetch_add(1); });
        std::string error;
        pipeline.start(&error);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // Destructor must stop threads without hanging.
    }
    CHECK(guiCount.load() > 0);
}

void testStartFailureAllowsRestart()
{
    media::MediaPipeline pipeline;
    // First start without source fails.
    std::string error;
    CHECK(!pipeline.start(&error));
    // Now set a source and start again.
    pipeline.setSource(std::make_unique<media::TestPatternSource>(160, 120, 30));
    CHECK(pipeline.start(&error));
    pipeline.stop();
}

void testStatisticsReportFramesDropsAndLatency()
{
    media::MediaPipeline pipeline(1);
    pipeline.setSource(std::make_unique<LimitedSource>(20, 160, 120, 0));
    pipeline.setGuiConsumer([](const media::FramePacket &) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    });

    std::string error;
    CHECK(pipeline.start(&error));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    pipeline.stop();

    const media::MediaPipelineStatistics stats = pipeline.statistics();
    CHECK(stats.queueCapacity == 1);
    CHECK(stats.sourceFrames == 20);
    CHECK(stats.sourceFps > 0.0);
    CHECK(stats.gui.processedFrames > 0);
    CHECK(stats.gui.droppedFrames > 0);
    CHECK(stats.gui.maxQueueDepth <= 1);
    CHECK(stats.gui.averageProcessingMs >= 4.0);
    CHECK(stats.gui.p95ProcessingMs >= 4.0);
    CHECK(stats.rknn.processedFrames == 0);
    CHECK(stats.rtsp.processedFrames == 0);
}

void testLaneCanBeDisabledAndReenabledWhileRunning()
{
    media::MediaPipeline pipeline(2);
    pipeline.setSource(
        std::make_unique<media::TestPatternSource>(64, 48, 100));
    std::atomic<int> guiFrames{0};
    pipeline.setGuiConsumer([&guiFrames](const media::FramePacket &) {
        ++guiFrames;
    });

    std::string error;
    CHECK(pipeline.start(&error));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(guiFrames.load() > 0);

    pipeline.setLaneEnabled(media::ConsumerLane::Gui, false);
    const int countAfterDisable = guiFrames.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(guiFrames.load() <= countAfterDisable + 1);

    pipeline.setLaneEnabled(media::ConsumerLane::Gui, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(guiFrames.load() > countAfterDisable + 1);
    pipeline.stop();
}

} // namespace

int main()
{
    testStartFailsWithoutSource();
    testConfiguredQueueCapacityIsApplied();
    testStartSucceedsWithSourceAndOpen();
    testConsumersReceiveFrames();
    testStopUnblocksAllThreads();
    testRepeatedStartStop();
    testStopIsIdempotent();
    testDisabledLaneDoesNotBlockOthers();
    testDestructorJoinsThreads();
    testStartFailureAllowsRestart();
    testStatisticsReportFramesDropsAndLatency();
    testLaneCanBeDisabledAndReenabledWhileRunning();

    if (g_failures == 0) {
        std::puts("media_pipeline tests passed");
        return 0;
    }
    std::printf("media_pipeline tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
