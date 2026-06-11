#include "SocketWrapper.hpp"
#include "common/config.hpp"

#include <algorithm>
#include <cstring>
#include <thread>
#include <chrono>

SocketWrapper::SocketWrapper() : lastActivityTime_(std::chrono::system_clock::now()) {}

TcpSocketStatus SocketWrapper::startOperatorServer(int port) {
    if (opServer_.status() >= tcpSOCKET_CONNECTED) {
        SOCKET_OP_ERR("Operator server is already running.");
        return tcpSOCKET_ERROR;
    }
    if (port < 1024 || port > 65535) {
        SOCKET_OP_ERR("Invalid port number: " << port);
        return tcpSOCKET_ERROR;
    }
    opServer_.setPort(port);
    opServer_.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return opServer_.status();
}

TcpSocketStatus SocketWrapper::stopOperatorServer() {
    opServer_.stop();
    return opServer_.status();
}

void SocketWrapper::attachOperatorServerCallback(OperatorCallback callback) {
    opServer_.setCallback(std::move(callback));
}

void SocketWrapper::attachAttackInterfaceCallback(AttackCallback callback) {
    attackServer_.setCallback(std::move(callback));
}

TcpSocketStatus SocketWrapper::startAttackInterfaceServer(int port) {
    if (attackServer_.status() >= tcpSOCKET_CONNECTED) {
        SOCKET_AT_ERR("Attack interface server is already running.");
        return tcpSOCKET_ERROR;
    }
    if (port < 1024 || port > 65535) {
        SOCKET_AT_ERR("Invalid port number: " << port);
        return tcpSOCKET_ERROR;
    }
    attackServer_.setPort(port);
    attackServer_.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return attackServer_.status();
}

TcpSocketStatus SocketWrapper::stopAttackInterfaceServer() {
    attackServer_.stop();
    return attackServer_.status();
}

void SocketWrapper::AttackInterfaceServer::txData(const std::shared_ptr<void>& data, size_t dataSize) {
    zmq::message_t message(dataSize);
    std::memcpy(message.data(), data.get(), dataSize);
    socket_->send(message, zmq::send_flags::dontwait);
}

void SocketWrapper::txAttackInterfaceData(const std::shared_ptr<void>& data, size_t dataSize) {
    attackServer_.txData(data, dataSize);
}

void SocketWrapper::attachDataHistorianCallback(DataHistorianCallback callback) {
    dataHistorianServer_.setCallback(std::move(callback));
}

TcpSocketStatus SocketWrapper::startDataHistorianServer(int port) {
    if (dataHistorianServer_.status() >= tcpSOCKET_CONNECTED) {
        SOCKET_DH_ERR("Data historian server is already running.");
        return tcpSOCKET_ERROR;
    }
    if (port < 1024 || port > 65535) {
        SOCKET_DH_ERR("Invalid port number: " << port);
        return tcpSOCKET_ERROR;
    }

    dataHistorianServer_.setPort(port);
    dataHistorianServer_.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return dataHistorianServer_.status();
}

TcpSocketStatus SocketWrapper::stopDataHistorianServer() {
    dataHistorianServer_.stop();
    return dataHistorianServer_.status();
}
