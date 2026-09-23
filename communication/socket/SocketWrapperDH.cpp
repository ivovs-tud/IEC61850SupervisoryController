#include "SocketWrapper.hpp"

#include "common/DataHistorian.hpp"
#include "common/config.hpp"

#include <utility>

namespace {

TcpServer::Config makeDataHistorianTcpConfig() {
    TcpServer::Config config;
    config.bindAddress = "0.0.0.0";
    config.port = 9003;
    config.maxClients = 1;
    config.receiveChunkBytes = 1024;
    config.transmitBufferBytes = 0;
    config.idleTimeout = std::chrono::milliseconds(2000);
    return config;
}

} // namespace

SocketWrapper::DataHistorianServer::DataHistorianServer(std::chrono::milliseconds pollPeriod)
    : PeriodicTask(pollPeriod), tcpServer_(makeDataHistorianTcpConfig()) {
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
        SOCKET_DH_ERR("Rejecting data historian client: " << reason);
    });
}

void SocketWrapper::DataHistorianServer::setPort(int port) {
    tcpServer_.setPort(port);
}

void SocketWrapper::DataHistorianServer::setCallback(DataHistorianCallback cb) {
    callback_ = std::move(cb);
}

tcpSocketStatus SocketWrapper::DataHistorianServer::status() const {
    return status_.load();
}

void SocketWrapper::DataHistorianServer::onStart() {
    receiveBuffers_.clear();
    status_.store(tcpSOCKET_CONNECTING);
    try {
        tcpServer_.start();
    } catch (...) {
        status_.store(tcpSOCKET_ERROR);
        throw;
    }
    status_.store(tcpSOCKET_CONNECTED);
    SOCKET_DH_ST("Data historian server listening");
}

void SocketWrapper::DataHistorianServer::execute() {
    if (status_.load() >= tcpSOCKET_CONNECTED) tcpServer_.poll();
}

void SocketWrapper::DataHistorianServer::onStop() {
    tcpServer_.stop();
    receiveBuffers_.clear();
    status_.store(tcpSOCKET_CLOSED);
    SOCKET_DH_ST("DataHistorian Socket Stopped");
}

void SocketWrapper::DataHistorianServer::clientConnected(TcpServer::ClientId clientId) {
    receiveBuffers_.emplace(clientId, std::vector<uint8_t>{});
    SOCKET_DH_ST("Accepted data historian client");
}

void SocketWrapper::DataHistorianServer::bytesReceived(
    TcpServer::ClientId clientId, const uint8_t* data, std::size_t size) {
    auto buffer = receiveBuffers_.find(clientId);
    if (buffer == receiveBuffers_.end()) return;
    buffer->second.insert(buffer->second.end(), data, data + size);

    // TCP read boundaries are unrelated to the fixed-size legacy records.
    while (buffer->second.size() >= sizeof(DH_TCP_DATA)) {
        if (callback_) callback_(buffer->second.data(), sizeof(DH_TCP_DATA));
        buffer->second.erase(buffer->second.begin(), buffer->second.begin() + sizeof(DH_TCP_DATA));
    }
}

void SocketWrapper::DataHistorianServer::clientDisconnected(
    TcpServer::ClientId clientId, const std::string& reason) {
    receiveBuffers_.erase(clientId);
    SOCKET_DH_ST("Data historian client disconnected: " << reason);
}
