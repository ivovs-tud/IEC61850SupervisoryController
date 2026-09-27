#pragma once

#include "communication/socket/socket_platform.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class TcpServer {
public:
    using ClientId = std::uint64_t;
    using ConnectedHandler = std::function<void(ClientId)>;
    using DataHandler = std::function<void(ClientId, const uint8_t*, std::size_t)>;
    using DisconnectedHandler = std::function<void(ClientId, const std::string&)>;
    using RejectedHandler = std::function<void(const std::string&)>;

    struct Config {
        std::string bindAddress{"0.0.0.0"};
        int port{0};
        std::size_t maxClients{1};
        std::size_t receiveChunkBytes{4096};
        std::size_t transmitBufferBytes{64 * 1024};
        bool tcpNoDelay{false};
        bool keepAlive{false};
        std::chrono::milliseconds tcpUserTimeout{0};
        std::chrono::milliseconds idleTimeout{0};
    };

    explicit TcpServer(Config config);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void setPort(int port);
    void setConnectedHandler(ConnectedHandler handler);
    void setDataHandler(DataHandler handler);
    void setDisconnectedHandler(DisconnectedHandler handler);
    void setRejectedHandler(RejectedHandler handler);

    void start();
    void poll();
    void stop() noexcept;

    bool send(ClientId clientId, const uint8_t* data, std::size_t size);
    void disconnect(ClientId clientId, std::string reason);
    bool hasClient(ClientId clientId) const;
    std::size_t clientCount() const;
    std::size_t queuedBytes(ClientId clientId) const;

private:
    struct Connection {
        ClientId id{0};
        socket_t fd{INVALID_SOCKET_FD};
        std::chrono::steady_clock::time_point lastReceive{};
        std::deque<std::vector<uint8_t>> outboundQueue;
        std::size_t queuedBytes{0};
        std::size_t sendOffset{0};
        bool closeRequested{false};
        std::string closeReason;
    };

    struct PolledClient {
        ClientId id{0};
        socket_t fd{INVALID_SOCKET_FD};
    };

    void acceptClients();
    bool configureClient(socket_t clientFd) const;
    void readClient(const PolledClient& client);
    void drainOutbound(const PolledClient& client);
    void drainAllOutbound();
    void expireIdleClients();
    void closeRequestedClients();
    void closeClient(ClientId clientId, const std::string& reason);
    bool isCurrentClient(const PolledClient& client) const;
    bool isCloseRequested(ClientId clientId) const;

    Config config_;
    socket_t listenerFd_{INVALID_SOCKET_FD};
    bool socketInitialized_{false};
    ClientId nextClientId_{1};
    std::vector<uint8_t> receiveBuffer_;

    ConnectedHandler connectedHandler_;
    DataHandler dataHandler_;
    DisconnectedHandler disconnectedHandler_;
    RejectedHandler rejectedHandler_;

    mutable std::mutex connectionsMutex_;
    std::vector<Connection> connections_;
};
