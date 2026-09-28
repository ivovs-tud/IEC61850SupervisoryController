#pragma once

#include "sc/communication/data_historian/DataHistorianRecord.hpp"
#include "sc/communication/network/TcpServer.hpp"
#include "sc/runtime/PeriodicTask.hpp"

#include <chrono>
#include <functional>
#include <unordered_map>
#include <vector>

class DataHistorianServer final : public PeriodicTask {
public:
    struct Config {
        int port{9003};
        std::chrono::milliseconds pollPeriod{10};
    };

    using RecordHandler = std::function<void(const DataHistorianRecord&)>;

    explicit DataHistorianServer(Config config);
    ~DataHistorianServer() override { stop(); }

    void setRecordHandler(RecordHandler handler);

protected:
    void onStart() override;
    void execute() override;
    void onStop() override;

private:
    void clientConnected(TcpServer::ClientId clientId);
    void bytesReceived(TcpServer::ClientId clientId, const uint8_t* data, std::size_t size);
    void clientDisconnected(TcpServer::ClientId clientId, const std::string& reason);

    Config config_;
    TcpServer tcpServer_;
    RecordHandler recordHandler_;
    std::unordered_map<TcpServer::ClientId, std::vector<uint8_t>> receiveBuffers_;
};
