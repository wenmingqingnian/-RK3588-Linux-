#include "dmesg_tail.h"
#include "logmanager.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <string>
#include <thread>

#if defined(__linux__)
#  include <fcntl.h>
#  include <poll.h>
#  include <unistd.h>
#  if defined(__GLIBC__) && defined(PR_SET_NAME)
#    include <sys/prctl.h>
#  endif
#endif

namespace vehiclelog {

DmesgTail::DmesgTail() = default;

DmesgTail::~DmesgTail()
{
    stop();
}

bool DmesgTail::start()
{
    if (m_running.load(std::memory_order_relaxed))
        return false;
    if (m_thread.joinable())
        m_thread.join();

#if !defined(__linux__)
    return false;
#else
    // 创建 self-pipe 用于可靠中断 poll()
    int pipefd[2] = {-1, -1};
    if (::pipe(pipefd) != 0) {
        VLOG_WARN("kernel", "DmesgTail: pipe() failed (%s), kernel logs not captured",
                  std::strerror(errno));
        return false;
    }
    // 两端都设非阻塞
    for (int i = 0; i < 2; ++i) {
        int fl = ::fcntl(pipefd[i], F_GETFL, 0);
        if (fl >= 0)
            ::fcntl(pipefd[i], F_SETFL, fl | O_NONBLOCK);
    }
    m_stopPipe[0] = pipefd[0];  // 读端
    m_stopPipe[1] = pipefd[1];  // 写端

    m_stopRequested.store(false, std::memory_order_relaxed);
    // 先标记 running 再启动线程，避免 start() 返回后 isRunning() 仍为 false
    m_running.store(true, std::memory_order_relaxed);
    m_thread = std::thread([this]() { run(); });
    return true;
#endif
}

void DmesgTail::stop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);

#if defined(__linux__)
    // 通过 self-pipe 唤醒可能阻塞在 poll() 的线程
    if (m_stopPipe[1] >= 0) {
        char c = 'x';
        ssize_t wr = ::write(m_stopPipe[1], &c, 1);
        (void)wr; // 即使 EAGAIN（管道满）poll 也会唤醒
    }
#endif

    if (m_thread.joinable())
        m_thread.join();

#if defined(__linux__)
    // 关闭 self-pipe
    for (int i = 0; i < 2; ++i) {
        if (m_stopPipe[i] >= 0) {
            ::close(m_stopPipe[i]);
            m_stopPipe[i] = -1;
        }
    }
#endif
    m_running.store(false, std::memory_order_relaxed);
}

bool DmesgTail::isRunning() const
{
    return m_running.load(std::memory_order_relaxed);
}

#if defined(__linux__)

// 主路径：dmesg --follow --since "now" 跳过历史，只捕获新消息。
// 用 --since 而非 /dev/kmsg 的 lseek(SEEK_END)，因为后者在某些内核
// 版本上行为不一致（poll 不返回）。
// 若 dmesg 不支持 --since，回退到纯 --follow（会刷历史，但功能正确）。
static FILE *openDmesgFollow()
{
    // 优先尝试带 --since 的命令（跳过历史）
    FILE *f = ::popen("dmesg --follow --since now --time-format=iso 2>/dev/null", "r");
    if (f)
        return f;
    // 回退：纯 --follow
    return ::popen("dmesg --follow --time-format=iso 2>/dev/null", "r");
}

void DmesgTail::run()
{
#if defined(PR_SET_NAME)
    prctl(PR_SET_NAME, "dmesg_tail", 0, 0, 0);
#endif

    FILE *dmesgPipe = openDmesgFollow();
    if (!dmesgPipe) {
        VLOG_WARN("kernel", "DmesgTail: dmesg --follow failed, kernel logs not captured");
        m_running.store(false, std::memory_order_relaxed);
        return;
    }

    int dataFd = ::fileno(dmesgPipe);
    if (dataFd < 0) {
        ::pclose(dmesgPipe);
        m_running.store(false, std::memory_order_relaxed);
        return;
    }
    // 设置非阻塞
    int flags = ::fcntl(dataFd, F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(dataFd, F_SETFL, flags | O_NONBLOCK);

    VLOG_INFO("kernel", "DmesgTail: started (dmesg --follow)");

    char buf[2048];
    std::string pending;

    while (!m_stopRequested.load(std::memory_order_relaxed)) {
        struct pollfd pfds[2];
        pfds[0].fd = dataFd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = m_stopPipe[0];
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        int ret = ::poll(pfds, 2, 200);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pfds[1].revents & POLLIN)
            break;  // stop() 被调用
        if (ret == 0)
            continue;

        if (pfds[0].revents & (POLLERR | POLLNVAL | POLLHUP))
            break;
        if (!(pfds[0].revents & POLLIN))
            continue;

        ssize_t n = ::read(dataFd, buf, sizeof(buf) - 1);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }
        if (n == 0)
            break;

        buf[n] = '\0';
        pending.append(buf);

        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            if (!line.empty())
                VLOG_INFO("kernel", "%s", line.c_str());
        }
    }

    if (!pending.empty() && !m_stopRequested.load(std::memory_order_relaxed)) {
        if (pending.back() == '\r')
            pending.pop_back();
        if (!pending.empty())
            VLOG_INFO("kernel", "%s", pending.c_str());
    }

    ::pclose(dmesgPipe);
    m_running.store(false, std::memory_order_relaxed);
}
#else
void DmesgTail::run() {}
#endif

} // namespace vehiclelog
