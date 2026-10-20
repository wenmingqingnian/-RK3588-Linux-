#include "frame_broker.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace media {

FrameBroker::FrameBroker(int capacity)
    : m_capacity(capacity > 0 ? capacity : 3)
{
    // Pre-allocate each lane's ring buffer to the exact capacity. The vector
    // size never changes after this point — publish/take move-assign in/out
    // of existing slots, so steady-state operation does zero heap allocation.
    // FramePacket is default-constructed in each slot initially; the first
    // publish move-assigns over it.
    for (int i = 0; i < static_cast<int>(ConsumerLane::kCount); ++i) {
        m_lanes[i].ringSlots.resize(static_cast<std::size_t>(m_capacity));
    }
}

FrameBroker::~FrameBroker()
{
    requestStop();
}

int FrameBroker::publish(const FramePacket &frame)
{
    int accepted = 0;
    const int n = static_cast<int>(ConsumerLane::kCount);
    for (int i = 0; i < n; ++i) {
        LaneState &lane = m_lanes[i];
        std::lock_guard<std::mutex> lock(lane.mutex);
        if (!lane.enabled)
            continue;

        // drop_oldest: if the ring is full, advance head (discard oldest).
        // The discarded slot's FramePacket will be overwritten by the move
        // assign below — its shared_ptr refcount drops, freeing the buffer
        // if this was the last reference.
        if (lane.count >= m_capacity) {
            lane.ringSlots[lane.head] = FramePacket{};  // release old, reset slot
            lane.head = (lane.head + 1) % m_capacity;
            --lane.count;
            lane.dropped.fetch_add(1, std::memory_order_relaxed);
        }

        // Move-assign the incoming frame into the tail slot. The incoming
        // `frame` is a const reference from the caller, so we cannot move
        // from it directly — we copy-assign. (The copy is one shared_ptr
        // bump + 6 POD members; the underlying video buffer is NOT copied
        // because shared_ptr<const vector<uint8_t>> shares ownership.)
        lane.ringSlots[lane.tail] = frame;
        lane.tail = (lane.tail + 1) % m_capacity;
        ++lane.count;

        int observed = lane.maxDepth.load(std::memory_order_relaxed);
        while (lane.count > observed &&
               !lane.maxDepth.compare_exchange_weak(
                   observed, lane.count, std::memory_order_relaxed)) {
        }
        lane.cv.notify_one();
        ++accepted;
    }
    return accepted;
}

FrameReadResult FrameBroker::take(ConsumerLane laneEnum,
                                  FramePacket *frame,
                                  int timeoutMs)
{
    const int i = laneIndex(laneEnum);
    LaneState &lane = m_lanes[i];

    std::unique_lock<std::mutex> lock(lane.mutex);
    if (lane.count == 0) {
        if (m_stopRequested.load(std::memory_order_acquire)) {
            return FrameReadResult::Stopped;
        }
        // Wait until either a frame arrives, stop is requested, or timeout.
        lane.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                         [this, &lane]() {
            return lane.count > 0 ||
                   m_stopRequested.load(std::memory_order_acquire);
        });
    }

    if (lane.count > 0) {
        // Move the frame out of the slot into the caller's FramePacket,
        // then reset the slot to release the underlying buffer promptly
        // (the shared_ptr inside drops its refcount).
        *frame = std::move(lane.ringSlots[lane.head]);
        lane.ringSlots[lane.head] = FramePacket{};
        lane.head = (lane.head + 1) % m_capacity;
        --lane.count;
        return FrameReadResult::FrameReady;
    }
    if (m_stopRequested.load(std::memory_order_acquire)) {
        return FrameReadResult::Stopped;
    }
    return FrameReadResult::Timeout;
}

void FrameBroker::setLaneEnabled(ConsumerLane laneEnum, bool enabled)
{
    const int i = laneIndex(laneEnum);
    LaneState &lane = m_lanes[i];
    {
        std::lock_guard<std::mutex> lock(lane.mutex);
        lane.enabled = enabled;
        if (!enabled) {
            // Drop buffered frames so a re-enabled lane starts clean.
            // Reset each slot to release the shared_ptr refs, then reset
            // the ring indices — no vector resize needed.
            for (int j = 0; j < lane.count; ++j) {
                const int idx = (lane.head + j) % m_capacity;
                lane.ringSlots[idx] = FramePacket{};
            }
            lane.head = 0;
            lane.tail = 0;
            lane.count = 0;
        }
        lane.cv.notify_all();
    }
}

bool FrameBroker::isLaneEnabled(ConsumerLane laneEnum) const
{
    const int i = laneIndex(laneEnum);
    const LaneState &lane = m_lanes[i];
    std::lock_guard<std::mutex> lock(lane.mutex);
    return lane.enabled;
}

void FrameBroker::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
    const int n = static_cast<int>(ConsumerLane::kCount);
    for (int i = 0; i < n; ++i) {
        LaneState &lane = m_lanes[i];
        std::lock_guard<std::mutex> lock(lane.mutex);
        lane.cv.notify_all();
    }
}

void FrameBroker::reset()
{
    m_stopRequested.store(false, std::memory_order_release);
    const int n = static_cast<int>(ConsumerLane::kCount);
    for (int i = 0; i < n; ++i) {
        LaneState &lane = m_lanes[i];
        std::lock_guard<std::mutex> lock(lane.mutex);
        // Release any buffered frames, then reset ring indices.
        for (int j = 0; j < lane.count; ++j) {
            const int idx = (lane.head + j) % m_capacity;
            lane.ringSlots[idx] = FramePacket{};
        }
        lane.head = 0;
        lane.tail = 0;
        lane.count = 0;
        lane.dropped.store(0, std::memory_order_relaxed);
        lane.maxDepth.store(0, std::memory_order_relaxed);
        lane.cv.notify_all();
    }
}

int FrameBroker::droppedCount(ConsumerLane laneEnum) const
{
    const int i = laneIndex(laneEnum);
    return m_lanes[i].dropped.load(std::memory_order_relaxed);
}

int FrameBroker::queueDepth(ConsumerLane laneEnum) const
{
    const int i = laneIndex(laneEnum);
    const LaneState &lane = m_lanes[i];
    std::lock_guard<std::mutex> lock(lane.mutex);
    return lane.count;
}

int FrameBroker::maxQueueDepth(ConsumerLane laneEnum) const
{
    const int i = laneIndex(laneEnum);
    return m_lanes[i].maxDepth.load(std::memory_order_relaxed);
}

} // namespace media
