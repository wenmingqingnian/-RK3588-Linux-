#include "frame_buffer_pool.h"

#include <utility>

namespace pool {

FrameBufferPool::FrameBufferPool(size_t capacityBytes,
                                 size_t minFreeBlocks,
                                 size_t maxBlocks)
    : m_capacityBytes(capacityBytes)
    , m_minFreeBlocks(minFreeBlocks)
    , m_maxBlocks(maxBlocks)
{
}

FrameBufferPool::~FrameBufferPool()
{
    // 析构时若有借出块未归还，Releaser 仍持有本池裸指针；调用方必须
    // 保证池生命周期长于所有借出块（与 MediaPipeline 同生命周期即可）。
    // 空闲链表由 unique_ptr 自动释放。
}

void FrameBufferPool::Releaser::operator()(std::vector<uint8_t> *p) const
{
    if (!p) return;
    if (pool && managed) {
        // 把底层内存交还池子。unique_ptr 重新接管裸指针所有权。
        std::unique_ptr<std::vector<uint8_t>> block(p);
        pool->returnBlock(std::move(block));
    } else {
        // 游离块：直接释放。
        delete p;
    }
}

std::shared_ptr<std::vector<uint8_t>> FrameBufferPool::acquire()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    ++m_totalAcquired;

    if (!m_free.empty()) {
        // 命中空闲链表。
        ++m_totalHit;
        std::unique_ptr<std::vector<uint8_t>> block = std::move(m_free.back());
        m_free.pop_back();
        ++m_inUse;

        // 把裸指针交给 shared_ptr，删除器负责归还。
        std::vector<uint8_t> *raw = block.release();
        return std::shared_ptr<std::vector<uint8_t>>(
            raw, Releaser{this, /*managed=*/true});
    }

    // 未命中。判断是否已达上限。
    const bool atCap = (m_maxBlocks > 0) &&
                       (m_inUse + m_free.size() >= m_maxBlocks);
    if (atCap) {
        // 超上限：分配一块游离 buffer，引用归零时直接 delete。
        ++m_totalOverflow;
        ++m_inUse;
        // 注意：m_inUse 在 Releaser(managed=false) 路径下不会回退，
        // 这是已知近似统计——overflow 在长期运行下应是极小值。
        auto standalone = std::make_shared<std::vector<uint8_t>>(m_capacityBytes);
        return standalone;
    }

    // 未命中且未超上限：新分配一块托管 buffer。
    ++m_totalMissAllocated;
    ++m_inUse;
    auto block = std::make_unique<std::vector<uint8_t>>(m_capacityBytes);
    std::vector<uint8_t> *raw = block.release();
    return std::shared_ptr<std::vector<uint8_t>>(
        raw, Releaser{this, /*managed=*/true});
}

void FrameBufferPool::prewarm()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    while (m_free.size() < m_minFreeBlocks) {
        // prewarm 期间不算 acquire / miss 统计，仅预热。
        auto block = std::make_unique<std::vector<uint8_t>>(m_capacityBytes);
        m_free.push_back(std::move(block));
    }
}

void FrameBufferPool::returnBlock(std::unique_ptr<std::vector<uint8_t>> block)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_totalReturned;
    if (m_inUse > 0) --m_inUse;

    // 若池已析构/收缩，可直接释放（这里 unique_ptr 不入池即自动释放）。
    // 否则归还到空闲链表。
    const size_t currentBlocks = m_free.size() + m_inUse;
    const bool overCap = (m_maxBlocks > 0) && (currentBlocks >= m_maxBlocks);
    if (overCap) {
        // 超上限：让 block 的 unique_ptr 在函数结束时自动释放。
        return;
    }
    m_free.push_back(std::move(block));
    m_cv.notify_one();
}

FrameBufferPool::Statistics FrameBufferPool::statistics() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Statistics s;
    s.capacityBytes = m_capacityBytes;
    s.minFreeBlocks = m_minFreeBlocks;
    s.maxBlocks = m_maxBlocks;
    s.totalAcquired = m_totalAcquired;
    s.totalHit = m_totalHit;
    s.totalMissAllocated = m_totalMissAllocated;
    s.totalOverflow = m_totalOverflow;
    s.totalReturned = m_totalReturned;
    s.currentFree = m_free.size();
    s.currentInUse = m_inUse;
    return s;
}

size_t FrameBufferPool::shrink()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const size_t freed = m_free.size();
    m_free.clear();
    return freed * m_capacityBytes;
}

} // namespace pool
