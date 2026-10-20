#ifndef VEHICLELOG_DMESG_TAIL_H
#define VEHICLELOG_DMESG_TAIL_H

#include <atomic>
#include <thread>

namespace vehiclelog {

// DmesgTail —— 后台线程持续读取内核日志并转发到 LogManager。
//
// 实现方式：优先用 /dev/kmsg（lseek SEEK_END 跳过历史缓冲，只捕获
// 应用启动后的新内核消息）；失败回退到 `dmesg --follow`。
//
// 停止机制：使用 self-pipe 模式——stop() 向管道写 1 字节，poll() 立即
// 唤醒退出。这是中断 poll() 的标准可靠方法，不依赖 close(fd) 的未定义行为。
//
// 线程安全：非线程安全。start()/stop() 必须由同一线程调用。
// 平台：Linux 上工作；非 Linux 平台 start() 返回 false（保持跨平台编译）。
// 生命周期：RAII，析构自动 stop()。
class DmesgTail {
public:
    DmesgTail();
    ~DmesgTail();

    DmesgTail(const DmesgTail &) = delete;
    DmesgTail &operator=(const DmesgTail &) = delete;

    bool start();
    void stop();
    bool isRunning() const;

private:
    void run();

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::thread m_thread;

    // self-pipe：m_stopPipe[0]=读端（poll 监听），m_stopPipe[1]=写端（stop 时写入）
    // stop() 写 1 字节到 [1]，run() 的 poll() 监听 [0]，立即唤醒退出。
    int m_stopPipe[2] = {-1, -1};
};

} // namespace vehiclelog

#endif // VEHICLELOG_DMESG_TAIL_H
