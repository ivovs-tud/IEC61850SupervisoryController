#include "SocketWrapper.hpp"
#include "SocketWrapper.hpp"
#include "common/config.hpp"

#include <algorithm>
#include <cstring>
#include <thread>
#include <chrono>

// ---------------------------------------------------------------------------
// OperatorServer
// ---------------------------------------------------------------------------

SocketWrapper::OperatorServer::OperatorServer(std::chrono::milliseconds pollPeriod) : PeriodicTask(pollPeriod) {}

void SocketWrapper::OperatorServer::setPort(int port) { port_ = port; }
void SocketWrapper::OperatorServer::setCallback(OperatorCallback cb) { callback_ = std::move(cb); }
tcpSocketStatus SocketWrapper::OperatorServer::status() const { return status_.load(); }

void SocketWrapper::OperatorServer::onStart() {
    try {
        socket_.emplace(context_, zmq::socket_type::pair);
        socket_->set(zmq::sockopt::rcvhwm, 3);
        socket_->bind("tcp://*:" + std::to_string(port_));
        SOCKET_OP_ST("Operator server listening on port " << port_);
        status_.store(tcpSOCKET_CONNECTED);
    } catch (const std::exception& e) {
        SOCKET_OP_ERR("Failed to start operator server on port " << port_ << ": " << e.what());
        status_.store(tcpSOCKET_ERROR);
    }
}

void SocketWrapper::OperatorServer::execute() {
    if (!socket_ || status_.load() < tcpSOCKET_CONNECTED) return;

    zmq::message_t message;
    const auto result = socket_->recv(message, zmq::recv_flags::dontwait);
    if (!result) return;  // no message this cycle

    SOCKET_OP_LOG_V2("Received a message of size " << message.size() << " bytes");

    if (callback_) {
        try {
            SOCKET_OP_LOG_V2("Unpacked vector of size " << message.size());
            callback_(static_cast<const uint8_t*>(message.data()), message.size());
        } catch (const std::exception& e) {
            SOCKET_OP_ERR("Failed to unpack message: " << e.what());
        }
    }
}

void SocketWrapper::OperatorServer::onStop() {
    socket_.reset();  // close socket before context is destroyed
    status_.store(tcpSOCKET_CLOSED);
    SOCKET_OP_ST("Operator server stopped on port " << port_);
}