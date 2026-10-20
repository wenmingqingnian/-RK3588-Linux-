#include "ZmqInterface.h"

namespace zmq_component {

ZmqCommunicationError::ZmqCommunicationError(const std::string& what)
    : std::runtime_error("ZMQ Error: " + what) {}

void ZmqInterface::setupSocket(int socket_type, const std::string& address) {
    try {
        context_ = std::make_unique<zmq::context_t>(1);
        socket_ = std::make_unique<zmq::socket_t>(*context_, socket_type);

        socket_->setsockopt(ZMQ_RCVTIMEO, &timeout_ms_, sizeof(timeout_ms_));
        socket_->setsockopt(ZMQ_SNDTIMEO, &timeout_ms_, sizeof(timeout_ms_));

        // ZMQ_LINGER=0: on close(), discard any unsent messages immediately
        // and return without blocking. Without this, the default (-1 = wait
        // forever) can hang TTS service shutdown, causing the parent process
        // to escalate to SIGKILL and leaving the TCP port in TIME_WAIT. This
        // makes the next CarTerminal launch fail with EADDRINUSE on the
        // 7777/6677 bind. LINGER=0 is appropriate for real-time voice/TTS
        // traffic where stale messages have no value.
        const int linger_ms = 0;
        socket_->setsockopt(ZMQ_LINGER, &linger_ms, sizeof(linger_ms));

        (socket_type == ZMQ_REP) ? socket_->bind(address)
                                  : socket_->connect(address);
    } catch (const zmq::error_t& e) {
        throw ZmqCommunicationError(e.what());
    }
}

ZmqInterface::~ZmqInterface() {
    if (socket_) socket_->close();
    if (context_) context_->close();
}

void ZmqInterface::setTimeout(int milliseconds) {
    timeout_ms_ = milliseconds;
    if (socket_) {
        socket_->setsockopt(ZMQ_RCVTIMEO, &timeout_ms_, sizeof(timeout_ms_));
        socket_->setsockopt(ZMQ_SNDTIMEO, &timeout_ms_, sizeof(timeout_ms_));
    }
}

} // namespace zmq_component
