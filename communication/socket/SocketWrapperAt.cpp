#include "SocketWrapper.hpp"
#include "SocketWrapper.hpp"
#include "common/config.hpp"

#include <algorithm>
#include <cstring>
#include <thread>
#include <chrono>

SocketWrapper::AttackInterfaceServer::AttackInterfaceServer(std::chrono::milliseconds pollPeriod) : PeriodicTask(pollPeriod) {}

void SocketWrapper::AttackInterfaceServer::setPort(int port) { port_ = port; }
tcpSocketStatus SocketWrapper::AttackInterfaceServer::status() const { return status_.load(); }
void SocketWrapper::AttackInterfaceServer::setCallback(AttackCallback cb) {callback_ = std::move(cb); }

void SocketWrapper::AttackInterfaceServer::onStart() {
    try {
        socket_.emplace(context_, zmq::socket_type::pair);
        socket_->set(zmq::sockopt::rcvhwm, 3);
        socket_->bind("tcp://*:" + std::to_string(port_));
        SOCKET_AT_ST("Attack interface server listening on port " << port_);
        status_.store(tcpSOCKET_CONNECTED);
    } catch (const std::exception& e) {
        SOCKET_AT_ERR("Failed to start attack interface server on port " << port_ << ": " << e.what());
        status_.store(tcpSOCKET_ERROR);
    }
}

void SocketWrapper::AttackInterfaceServer::execute() {
    if (!socket_ || status_.load() < tcpSOCKET_CONNECTED) return;

    zmq::message_t message;
    const auto result = socket_->recv(message, zmq::recv_flags::dontwait);
    if (!result) return;

    SOCKET_AT_LOG_V2("Received a message of size " << message.size() << " bytes");

    if (callback_) {
        try {
            SOCKET_AT_LOG_V2("Passing payload of size " << message.size() << " to callback");
            callback_(static_cast<const uint8_t*>(message.data()), message.size());
        } catch (const std::exception& e) {
            SOCKET_AT_ERR("Failed to unpack message: " << e.what());
        }
    }
}

void SocketWrapper::AttackInterfaceServer::onStop() {
    socket_.reset();
    status_.store(tcpSOCKET_CLOSED);
    SOCKET_AT_ST("Attack interface server stopped on port " << port_);
}