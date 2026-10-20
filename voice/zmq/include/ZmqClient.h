#pragma once
#include "ZmqInterface.h"
#include <mutex>

namespace zmq_component {

// ZMQ socket 并发安全约束：libzmq socket_t 必须由"创建它的线程"独占使用
// （见 libzmq 官方文档 thread-safety 章节）。并发 send/recv 会触发断言失败
// 或消息错乱。本类持有 mutex 串行化所有 socket 操作，使 ZmqClient 实例可
// 被多个 worker 线程并发调用。
class ZmqClient : public ZmqInterface {
public:
    explicit ZmqClient(const std::string& address = "tcp://localhost:6666");
    void sendRequest(const std::string& message);
    std::string receiveResponse();
    std::string request(const std::string& message);

private:
    std::mutex m_opMutex;  // 串行化 socket 操作，防止并发访问
};

} // namespace zmq_component