#include "thread_pool.h"

#include <algorithm>
#include <utility>

namespace concurrency {

ThreadPool::ThreadPool(size_t workerCount)
{
    if (workerCount == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        workerCount = (hw > 0) ? static_cast<size_t>(hw) : 1;
    }
    // 上限保护：RK3588S 八核足够，避免业务层误传超大值。
    workerCount = std::max<size_t>(1u, std::min<size_t>(workerCount, 32u));

    m_workers.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
        m_workers.emplace_back([this, i]() { this->workerLoop(i); });
    }
}

ThreadPool::~ThreadPool()
{
    stop();
}

void ThreadPool::workerLoop(size_t /*workerId*/)
{
    for (;;) {
        PackagedTask task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_taskAvailable.wait(lock, [this]() {
                return m_stopping.load(std::memory_order_acquire) ||
                       !m_tasks.empty();
            });

            if (m_stopping.load(std::memory_order_acquire) &&
                m_tasks.empty()) {
                return;
            }

            task = std::move(m_tasks.front());
            m_tasks.pop();
        }

        m_activeWorkers.fetch_add(1, std::memory_order_relaxed);
        // 执行任务。packaged_task 内部已捕获异常并写入 future，
        // 不会冒泡到 worker 线程导致退出。
        task();
        m_activeWorkers.fetch_sub(1, std::memory_order_relaxed);
        m_completedTasks.fetch_add(1, std::memory_order_relaxed);

        // 通知 waitIdle：可能队列空了且无活跃 worker。
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_tasks.empty() &&
                m_activeWorkers.load(std::memory_order_relaxed) == 0) {
                m_idleOrStopped.notify_all();
            }
        }
    }
}

void ThreadPool::waitIdle()
{
    // 谓词同时感知 m_stopping，防止与 stop() 并发调用时陷入永久等待：
    // stop() 完成后 worker 已 join，再没有人会通知 m_idleOrStopped。
    std::unique_lock<std::mutex> lock(m_mutex);
    m_idleOrStopped.wait(lock, [this]() {
        return (m_tasks.empty() &&
                m_activeWorkers.load(std::memory_order_relaxed) == 0) ||
               m_stopping.load(std::memory_order_acquire);
    });
}

void ThreadPool::stop()
{
    if (m_stopping.exchange(true, std::memory_order_acq_rel)) {
        // 已停止。但 worker 可能尚未 join（重复调用 stop 场景）。
        // 仍要确保 join 已完成。
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_taskAvailable.notify_all();
        m_idleOrStopped.notify_all();
    }

    for (std::thread &w : m_workers) {
        if (w.joinable())
            w.join();
    }
    m_workers.clear();
}

ThreadPool::Statistics ThreadPool::statistics() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Statistics s;
    s.workerCount = m_workers.size();
    s.submittedTasks = m_submittedTasks.load(std::memory_order_relaxed);
    s.completedTasks = m_completedTasks.load(std::memory_order_relaxed);
    s.pendingTasks = m_tasks.size();
    s.activeWorkers = m_activeWorkers.load(std::memory_order_relaxed);
    return s;
}

} // namespace concurrency
