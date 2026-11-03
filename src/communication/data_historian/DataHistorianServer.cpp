#include "sc/communication/data_historian/DataHistorianServer.hpp"

#include "sc/runtime/Logging.hpp"

#include <cstring>
#include <stdexcept>
#include <utility>

namespace {

TcpServer::Config makeTcpConfig(int port)
{
    TcpServer::Config config;
    config.bindAddress = "0.0.0.0";
    config.port = port;
    config.maxClients = 1;
    config.receiveChunkBytes = 1024;
    config.transmitBufferBytes = 0;
    config.idleTimeout = std::chrono::milliseconds(2000);
    return config;
}

} // namespace

DataHistorianServer::DataHistorianServer(Config config) :
    PeriodicTask(config.pollPeriod), config_(config), tcpServer_(makeTcpConfig(config.port))
{
    if (config_.port < 1024 || config_.port > 65535) {
        throw std::invalid_argument("invalid data historian server port");
    }
    tcpServer_.setConnectedHandler([this](TcpServer::ClientId clientId) { clientConnected(clientId); });
    tcpServer_.setDataHandler(
        [this](TcpServer::ClientId clientId, const uint8_t* data, std::size_t size) { bytesReceived(clientId, data, size); });
    tcpServer_.setDisconnectedHandler(
        [this](TcpServer::ClientId clientId, const std::string& reason) { clientDisconnected(clientId, reason); });
    tcpServer_.setRejectedHandler([](const std::string& reason) { SOCKET_DH_ERR("Rejecting data historian client: " << reason); });
}

void DataHistorianServer::setRecordHandler(RecordHandler handler)
{
    recordHandler_ = std::move(handler);
}

void DataHistorianServer::onStart()
{
    receiveBuffers_.clear();
    tcpServer_.start();
    SOCKET_DH_ST("Data historian server listening on port " << config_.port);
}

void DataHistorianServer::execute()
{
    tcpServer_.poll();
}

void DataHistorianServer::onStop()
{
    tcpServer_.stop();
    receiveBuffers_.clear();
    SOCKET_DH_ST("Data historian server stopped on port " << config_.port);
}

void DataHistorianServer::clientConnected(TcpServer::ClientId clientId)
{
    receiveBuffers_.emplace(clientId, std::vector<uint8_t>{});
    SOCKET_DH_ST("Accepted data historian client");
}

void DataHistorianServer::bytesReceived(TcpServer::ClientId clientId, const uint8_t* data, std::size_t size)
{
    auto buffer = receiveBuffers_.find(clientId);
    if (buffer == receiveBuffers_.end())
        return;
    buffer->second.insert(buffer->second.end(), data, data + size);

    while (buffer->second.size() >= sizeof(DataHistorianRecord)) {
        DataHistorianRecord record{};
        std::memcpy(&record, buffer->second.data(), sizeof(record));
        if (recordHandler_)
            recordHandler_(record);
        buffer->second.erase(buffer->second.begin(), buffer->second.begin() + sizeof(DataHistorianRecord));
    }
}

void DataHistorianServer::clientDisconnected(TcpServer::ClientId clientId, const std::string& reason)
{
    receiveBuffers_.erase(clientId);
    SOCKET_DH_ST("Data historian client disconnected: " << reason);
}
