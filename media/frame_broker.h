#ifndef MEDIA_FRAME_BROKER_H
#define MEDIA_FRAME_BROKER_H

#include "frame_packet.h"
#include "frame_source.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace media {

enum class ConsumerLane {
    Gui = 0,
    Rknn = 1,
    Rtsp = 2,
    kCount = 3
};

// FrameBroker distributes frames from a single producer (the video source
// thread) to up to three independent consumer lanes (GUI, RKNN, RTSP). Each
// lane has its own bounded queue and drop_oldest policy: when a lane's queue
// is full, the oldest frame is discarded to make room for the newest.
//
// Disabled lanes (setLaneEnabled(false)) discard incoming frames immediately
// without buffering, so a closed RKNN/RTSP lane does not accumulate memory.
//
// Implementation: each lane is a fixed-capacity ring buffer
// (std::vector<FramePacket> + head/tail/count). Frames are moved (not
// copied) in/out of pre-allocated slots, so steady-state operation does
// zero heap allocation. The mutex is retained: at 1 producer + 3 consumers
// the lock contention is sub-microsecond and not a bottleneck, while a
// truly lock-free MPMC ring would add substantial complexity for no
// real-world gain (the shared_ptr atomic refcount inside FramePacket is
// itself not lock-free).
//
// Thread-safety: publish/setLaneEnabled/requestStop/reset may be called from
// any thread. take() is intended to be called from a single consumer thread
// per lane.
class FrameBroker {
public:
    // capacity is the per-lane queue depth (applied to all lanes).
    explicit FrameBroker(int capacity = 3);
    ~FrameBroker();

    FrameBroker(const FrameBroker &) = delete;
    FrameBroker &operator=(const FrameBroker &) = delete;

    // Publish a frame to all enabled lanes. Returns the number of lanes that
    // accepted the frame (i.e. were enabled). Disabled lanes drop the frame.
    int publish(const FramePacket &frame);

    // Take a frame from the specified lane. Blocks until a frame is
    // available, the timeout expires, or requestStop() is called.
    FrameReadResult take(ConsumerLane lane,
                         FramePacket *frame,
                         int timeoutMs);

    // Enable/disable a lane. Disabled lanes drop incoming frames.
    void setLaneEnabled(ConsumerLane lane, bool enabled);
    bool isLaneEnabled(ConsumerLane lane) const;

    // Wake all blocked consumers. After requestStop, take() returns Stopped
    // until reset() is called.
    void requestStop();

    // Reset to a fresh state: clears all queues, statistics, and the stop
    // flag. Safe to call after requestStop() to reuse the broker for a new
    // pipeline run.
    void reset();

    // Statistics.
    int droppedCount(ConsumerLane lane) const;
    int queueDepth(ConsumerLane lane) const;
    int maxQueueDepth(ConsumerLane lane) const;
    int capacity() const { return m_capacity; }

private:
    // Per-lane fixed-capacity ring buffer. Frames are moved (not copied)
    // in/out of `slots`; the vector's size never changes after construction,
    // so steady-state operation does zero heap allocation.
    struct LaneState {
        std::vector<FramePacket> ringSlots;   // size == capacity, pre-allocated
        int head = 0;                     // index of next frame to take
        int tail = 0;                     // index of next slot to write
        int count = 0;                    // current number of frames
        mutable std::mutex mutex;
        std::condition_variable cv;
        bool enabled = true;
        std::atomic<int> dropped{0};
        std::atomic<int> maxDepth{0};
    };

    int laneIndex(ConsumerLane lane) const {
        return static_cast<int>(lane);
    }

    const int m_capacity;
    std::atomic<bool> m_stopRequested{false};
    LaneState m_lanes[static_cast<int>(ConsumerLane::kCount)];
};

} // namespace media

#endif // MEDIA_FRAME_BROKER_H
