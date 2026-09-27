#pragma once

#include "PeriodicTask.hpp"
#include "TcpServer.hpp"
#include "sc/ports/AttackChannel.hpp"
#include "sc/protocol/AttackProtocol.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

class AttackChannelTCP final : public PeriodicTask, public sc::ports::AttackChannel {
public:
    struct Config {
        std::string bindAddress{"0.0.0.0"};
        int port{9002};
        std::chrono::milliseconds pollPeriod{10};
        std::size_t turbineCount{9};
        std::size_t receiveBufferBytes{sc::protocol::attack::DEFAULT_BUFFER_LIMIT};
        std::size_t transmitBufferBytes{sc::protocol::attack::DEFAULT_BUFFER_LIMIT};
        std::chrono::milliseconds tcpUserTimeout{0};
    };

    explicit AttackChannelTCP(Config config);
    ~AttackChannelTCP() override { stop(); }

    void setReceiveHandler(sc::ports::AttackReceiveHandler handler) override;
    void setLeaseCheckHandler(sc::ports::AttackLeaseCheckHandler handler) override;
    void setDisconnectHandler(sc::ports::AttackDisconnectHandler handler) override;
    bool send(const uint8_t* data, std::size_t size) override;

    bool hasClient() const { return activeClient_.load() != 0; }
    std::size_t queuedBytes() const;

protected:
    void onStart() override;
    void execute() override;
    void onStop() override;

private:
    void clientConnected(TcpServer::ClientId clientId);
    void bytesReceived(TcpServer::ClientId clientId, const uint8_t* data, std::size_t size);
    void clientDisconnected(TcpServer::ClientId clientId, const std::string& reason);

    Config config_;
    sc::protocol::attack::StreamDecoder decoder_;
    TcpServer tcpServer_;
    std::atomic<TcpServer::ClientId> activeClient_{0};

    sc::ports::AttackReceiveHandler receiveHandler_;
    sc::ports::AttackLeaseCheckHandler leaseCheckHandler_;
    sc::ports::AttackDisconnectHandler disconnectHandler_;
};
