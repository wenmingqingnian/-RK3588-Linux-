#ifndef COMMON_FRAME_BUFFER_POOL_H
#define COMMON_FRAME_BUFFER_POOL_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <vector>

namespace pool {

// FrameBufferPool —— 媒体管线帧缓冲内存池
//
// 设计目标（task: 工程优化与稳定性增强 - 内存池）：
//   1. 消除 FileFrameSource / TestPatternSource 等帧源每帧
//      `std::make_shared<std::vector<uint8_t>>(frameSize)` 反复 heap 分配。
//   2. 块大小固定（构造时指定 capacityBytes），从自由链表取/归还，
//      归零 malloc/free 调用，降低帧生产尾延迟抖动。
//   3. 返回 std::shared_ptr<std::vector<uint8_t>>，与 FramePacket::data
//      （shared_ptr<const vector<uint8_t>>）类型直接兼容；引用计数归零
//      时通过自定义删除器把底层 buffer 归还池子，对调用方零侵入。
//   4. 不依赖 Qt，可在 g++ -std=c++17 单测下独立运行。
//
// 线程安全：所有公有接口可从任意线程调用。
// 生命周期：建议长期持有（如 MediaPipeline 拥有一个），析构时归还所有块。
//
// 容量策略：
//   - minFreeBlocks:   空闲链表保底块数，低于该值时 acquire 会扩容。
//   - maxBlocks:       池上限，防止上游持续泄漏导致内存失控。
//   - 当 acquire 时池满且达上限：返回新分配的"未托管块"（不归还池），
//     保证调用方语义不变，仅在统计上记一次 overflow。
class FrameBufferPool {
public:
    struct Statistics {
        size_t capacityBytes = 0;       // 每块字节大小
        size_t minFreeBlocks = 0;       // 保底空闲块数
        size_t maxBlocks = 0;           // 池上限
        size_t totalAcquired = 0;       // 累计 acquire 调用次数
        size_t totalHit = 0;            // 命中空闲链表的次数
        size_t totalMissAllocated = 0;  // 未命中但未超上限、新分配的次数
        size_t totalOverflow = 0;       // 超过 maxBlocks 强制 heap 分配次数
        size_t totalReturned = 0;       // 累计归还次数
        size_t currentFree = 0;         // 当前空闲块数
        size_t currentInUse = 0;        // 当前借出块数
    };

    // capacityBytes: 每块字节数；minFreeBlocks: 保底空闲块数；
    // maxBlocks: 池上限（0 表示无上限，仅受 minFreeBlocks 约束）。
    FrameBufferPool(size_t capacityBytes,
                    size_t minFreeBlocks = 4,
                    size_t maxBlocks = 16);
    ~FrameBufferPool();

    FrameBufferPool(const FrameBufferPool &) = delete;
    FrameBufferPool &operator=(const FrameBufferPool &) = delete;

    // 借出一块。返回的 shared_ptr 在引用计数归零时自动归还到池中。
    // 调用方可直接写入数据；返回的 vector 大小 == capacityBytes。
    // 阻塞语义：非阻塞。若池空且未达上限则扩容；达上限则 heap 分配
    // 一块"游离 buffer"（不会归还到池，引用归零时直接释放）。
    std::shared_ptr<std::vector<uint8_t>> acquire();

    // 主动预分配至 minFreeBlocks 块。可在管线启动前调用以平摊首次抖动。
    void prewarm();

    // 当前统计快照。线程安全。
    Statistics statistics() const;

    // 释放所有空闲块（不影响已借出的块）。已借出块归还时若池已被
    // shrink 过则直接释放。返回释放的字节数。
    size_t shrink();

private:
    // 把一块 buffer 加入空闲链表（内部加锁；由 Releaser 在 shared_ptr
    // 引用归零时调用）。
    void returnBlock(std::unique_ptr<std::vector<uint8_t>> block);

    // shared_ptr 删除器需要引用池子，但又要避免环引用，故用裸指针。
    // 池生命周期由调用方保证（FrameBufferPool 析构前不能有借出块归还）。
    struct Releaser {
        FrameBufferPool *pool;
        bool managed;  // true: 归还到池；false: 直接 delete（游离块）
        void operator()(std::vector<uint8_t> *p) const;
    };

    const size_t m_capacityBytes;
    const size_t m_minFreeBlocks;
    const size_t m_maxBlocks;  // 0 = 无上限

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;  // 预留：可用于未来阻塞 acquire 变体
    std::vector<std::unique_ptr<std::vector<uint8_t>>> m_free;

    // 统计（持锁访问）
    size_t m_totalAcquired = 0;
    size_t m_totalHit = 0;
    size_t m_totalMissAllocated = 0;
    size_t m_totalOverflow = 0;
    size_t m_totalReturned = 0;
    size_t m_inUse = 0;  // 借出未归还的块数（含游离块）
};

} // namespace pool

#endif // COMMON_FRAME_BUFFER_POOL_H
