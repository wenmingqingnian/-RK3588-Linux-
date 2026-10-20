#include "ZmqClient.h"
#include <cstring>

namespace zmq_component {

ZmqClient::ZmqClient(const std::string& address) {
    setupSocket(ZMQ_REQ, address);
}

void ZmqClient::sendRequest(const std::string& message) {
    std::lock_guard<std::mutex> lock(m_opMutex);
    zmq::message_t request(message.data(), message.size());
    if (!socket_->send(request, zmq::send_flags::none)) {
        throw ZmqCommunicationError("Send timeout");
    }
}

std::string ZmqClient::receiveResponse() {
    std::lock_guard<std::mutex> lock(m_opMutex);
    zmq::message_t reply;
    if (!socket_->recv(reply)) {
        throw ZmqCommunicationError("Receive timeout");
    }
    return {static_cast<char*>(reply.data()), reply.size()};
}

std::string ZmqClient::request(const std::string& message) {
    // request() 串行化整个 send→recv 周期，保证 ZMQ_REQ 状态机一致性
    // （send 后必须先 recv 才能再 send）。
    std::lock_guard<std::mutex> lock(m_opMutex);
    zmq::message_t requestData(message.data(), message.size());
    if (!socket_->send(requestData, zmq::send_flags::none)) {
        throw ZmqCommunicationError("Send timeout");
    }
    zmq::message_t reply;
    if (!socket_->recv(reply)) {
        throw ZmqCommunicationError("Receive timeout");
    }
    return {static_cast<char*>(reply.data()), reply.size()};
}

} // namespace zmq_component