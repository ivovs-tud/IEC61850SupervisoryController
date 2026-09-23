#include "AttackChannelTCP.hpp"

#include "common/config.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace {

TcpServer::Config makeTcpConfig(const AttackChannelTCP::Config& config) {
    TcpServer::Config tcp;
    tcp.bindAddress = config.bindAddress;
    tcp.port = config.port;
    tcp.maxClients = 1;
    tcp.receiveChunkBytes = 4096;
    tcp.transmitBufferBytes = config.transmitBufferBytes;
    tcp.tcpNoDelay = true;
    tcp.keepAlive = true;
    tcp.tcpUserTimeout = config.tcpUserTimeout;
    return tcp;
}

} // namespace

AttackChannelTCP::AttackChannelTCP(Config config)
    : PeriodicTask(config.pollPeriod),
      config_(std::move(config)),
      decoder_(config_.turbineCount, config_.receiveBufferBytes),
      tcpServer_(makeTcpConfig(config_)) {
    tcpServer_.setConnectedHandler([this](TcpServer::ClientId clientId) {
        clientConnected(clientId);
    });
    tcpServer_.setDataHandler([this](TcpServer::ClientId clientId, const uint8_t* data, std::size_t size) {
        bytesReceived(clientId, data, size);
    });
    tcpServer_.setDisconnectedHandler([this](TcpServer::ClientId clientId, const std::string& reason) {
        clientDisconnected(clientId, reason);
    });
    tcpServer_.setRejectedHandler([](const std::string& reason) {
        (void)reason;
        SOCKET_AT_LOG_V1("Rejecting raw TCP attack client: " << reason);
    });
}

void AttackChannelTCP::setReceiveHandler(sc::ports::AttackReceiveHandler handler) {
    receiveHandler_ = std::move(handler);
}

void AttackChannelTCP::setLeaseCheckHandler(sc::ports::AttackLeaseCheckHandler handler) {
    leaseCheckHandler_ = std::move(handler);
}

void AttackChannelTCP::setDisconnectHandler(sc::ports::AttackDisconnectHandler handler) {
    disconnectHandler_ = std::move(handler);
}

bool AttackChannelTCP::send(const uint8_t* data, std::size_t size) {
    return tcpServer_.send(activeClient_.load(), data, size);
}

std::size_t AttackChannelTCP::queuedBytes() const {
    return tcpServer_.queuedBytes(activeClient_.load());
}

void AttackChannelTCP::onStart() {
    activeClient_.store(0);
    decoder_.reset();
    tcpServer_.start();
    SOCKET_AT_ST("Raw TCP attack interface listening on " << config_.bindAddress << ':' << config_.port);
}

void AttackChannelTCP::execute() {
    if (leaseCheckHandler_) leaseCheckHandler_();
    tcpServer_.poll();
}

void AttackChannelTCP::onStop() {
    tcpServer_.stop();
    activeClient_.store(0);
    decoder_.reset();
    SOCKET_AT_ST("Raw TCP attack interface stopped");
}

void AttackChannelTCP::clientConnected(TcpServer::ClientId clientId) {
    activeClient_.store(clientId);
    decoder_.reset();
    SOCKET_AT_ST("Accepted raw TCP attack client");
}

void AttackChannelTCP::bytesReceived(TcpServer::ClientId clientId, const uint8_t* data, std::size_t size) {
    if (clientId != activeClient_.load()) return;
    try {
        std::size_t offset = 0;
        while (offset < size) {
            const std::size_t available = config_.receiveBufferBytes - decoder_.bufferedBytes();
            if (available == 0) throw sc::protocol::attack::ProtocolError("attack receive buffer limit exceeded");
            const std::size_t chunkSize = std::min(available, size - offset);
            const auto messages = decoder_.push(data + offset, chunkSize);
            offset += chunkSize;
            for (const auto& message : messages) {
                if (receiveHandler_) receiveHandler_(message.data(), message.size());
            }
        }
    } catch (const std::exception& error) {
        tcpServer_.disconnect(clientId, std::string("protocol error: ") + error.what());
    }
}

void AttackChannelTCP::clientDisconnected(TcpServer::ClientId clientId, const std::string& reason) {
    TcpServer::ClientId expected = clientId;
    if (!activeClient_.compare_exchange_strong(expected, 0)) return;
    const std::string disconnectReason = decoder_.bufferedBytes() == 0
        ? reason
        : "protocol error: truncated attack message";
    decoder_.reset();
    SOCKET_AT_ST("Raw TCP attack client disconnected: " << disconnectReason);
    if (disconnectHandler_) disconnectHandler_(disconnectReason);
}
