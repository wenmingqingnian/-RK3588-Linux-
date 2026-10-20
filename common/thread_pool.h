#ifndef COMMON_THREAD_POOL_H
#define COMMON_THREAD_POOL_H

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <vector>

namespace concurrency {

// ThreadPool —— 通用固定 worker 数线程池
//
// 设计目标（task: 工程优化 - 短时 CPU 密集任务调度）：
//   1. 替代项目里"为每件事新开 std::thread"的写法，复用 worker，
//      避免 thread 创建/销毁开销。
//   2. 适用场景：RKNN 后处理(NMS)、RGA 失败时的 CPU 颜色转换回退、
//      V4L2 CPU fallback YUV→RGB、LLM 输出意图解析等。
//   3. 不适用：长生命周期独占线程（GPS/DHT11/AP3216 worker 应保持原
//      独占线程模型，不要切到池里）。
//
// 兼容性：C++17，不依赖 Qt、不依赖 C++20 jthread/stop_token。
//
// 生命周期：
//   - 构造时启动所有 worker；析构时 stop() + join 全部 worker。
//   - 析构前未取走的 future 不会阻塞池析构，但未启动的任务会被丢弃
//     （已 packaged，对应的 future 在销毁时变为 broken_promise）。
//
// 线程安全：所有公有接口可从任意线程调用。
class ThreadPool {
public:
    struct Statistics {
        size_t workerCount = 0;
        uint64_t submittedTasks = 0;
        uint64_t completedTasks = 0;
        size_t pendingTasks = 0;     // 当前队列中待执行任务数
        size_t activeWorkers = 0;    // 当前正在执行任务的 worker 数
    };

    // 若 workerCount 为 0，则取 std::thread::hardware_concurrency()，
    // 再兜底为 1。
    explicit ThreadPool(size_t workerCount = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    // 提交一个可调用对象。返回 future 以获取返回值或异常。
    // 若 stop() 已调用，返回一个已就绪（broken_promise）的 future。
    template <typename F, typename... Args>
    auto submit(F &&f, Args &&...args)
        -> std::future<std::invoke_result_t<F, Args...>>;

    // 等待所有已提交任务完成（队列空且所有 worker 空闲）。
    // 不阻止继续 submit；与 stop() 互斥调用，避免死锁。
    void waitIdle();

    // 停止接收新任务，唤醒所有 worker，join 全部线程。
    // 幂等；可在析构前主动调用。
    void stop();

    Statistics statistics() const;

private:
    // worker 主循环。workerId 仅用于统计与日志区分。
    void workerLoop(size_t workerId);

    // 类型擦除的任务包装：packaged_task<void()> 能持有任意返回值类型的
    // task 并把结果通过 future 传回。
    using PackagedTask = std::packaged_task<void()>;

    std::vector<std::thread> m_workers;
    std::queue<PackagedTask> m_tasks;

    mutable std::mutex m_mutex;
    std::condition_variable m_taskAvailable;   // worker 等待新任务
    std::condition_variable m_idleOrStopped;   // waitIdle 等待

    std::atomic<bool> m_stopping{false};
    std::atomic<size_t> m_activeWorkers{0};

    // 统计
    std::atomic<uint64_t> m_submittedTasks{0};
    std::atomic<uint64_t> m_completedTasks{0};
};

// ---- 模板实现（必须放头文件） ----------------------------------------

template <typename F, typename... Args>
auto ThreadPool::submit(F &&f, Args &&...args)
    -> std::future<std::invoke_result_t<F, Args...>>
{
    using ReturnType = std::invoke_result_t<F, Args...>;

    if (m_stopping.load(std::memory_order_acquire)) {
        // 已停止：返回一个 broken_promise 的 future。
        std::promise<ReturnType> broken;
        broken.set_exception(std::make_exception_ptr(
            std::runtime_error("ThreadPool: stopped")));
        return broken.get_future();
    }

    // 把 callable + args 绑定成一个无参可调用对象。
    auto bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
    auto task = std::make_shared<std::packaged_task<ReturnType()>>(
        std::move(bound));
    std::future<ReturnType> result = task->get_future();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 用 lambda 捕获 shared_ptr<packaged_task<ReturnType()>>，
        // 在内部转换为 packaged_task<void()> 容器可存储的形式。
        m_tasks.emplace([task]() { (*task)(); });
        m_submittedTasks.fetch_add(1, std::memory_order_relaxed);
    }
    m_taskAvailable.notify_one();
    return result;
}

} // namespace concurrency

#endif // COMMON_THREAD_POOL_H
