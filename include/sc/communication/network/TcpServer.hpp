#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

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
    class Impl;
    std::unique_ptr<Impl> impl_;
};
