#include "sc/communication/network/TcpServer.hpp"

#include "SocketPlatform.hpp"

#include <algorithm>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(PLATFORM_WINDOWS)
#include <mstcpip.h>
#else
#include <netinet/tcp.h>
#endif

class TcpServer::Impl {
    public:
    explicit Impl(Config config);
    ~Impl();

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

TcpServer::Impl::Impl(Config config) : config_(std::move(config)), receiveBuffer_(config_.receiveChunkBytes)
{
    if (config_.port < 0 || config_.port > 65535)
        throw std::invalid_argument("invalid TCP server port");
    if (config_.maxClients == 0)
        throw std::invalid_argument("TCP server requires at least one client slot");
    if (config_.receiveChunkBytes == 0)
        throw std::invalid_argument("TCP receive chunk must not be empty");
}

TcpServer::Impl::~Impl()
{
    stop();
}

void TcpServer::Impl::setPort(int port)
{
    if (listenerFd_ != INVALID_SOCKET_FD)
        throw std::logic_error("cannot change a running TCP server port");
    if (port < 0 || port > 65535)
        throw std::invalid_argument("invalid TCP server port");
    config_.port = port;
}

void TcpServer::Impl::setConnectedHandler(ConnectedHandler handler)
{
    connectedHandler_ = std::move(handler);
}

void TcpServer::Impl::setDataHandler(DataHandler handler)
{
    dataHandler_ = std::move(handler);
}

void TcpServer::Impl::setDisconnectedHandler(DisconnectedHandler handler)
{
    disconnectedHandler_ = std::move(handler);
}

void TcpServer::Impl::setRejectedHandler(RejectedHandler handler)
{
    rejectedHandler_ = std::move(handler);
}

void TcpServer::Impl::start()
{
    if (listenerFd_ != INVALID_SOCKET_FD)
        throw std::logic_error("TCP server is already running");
    if (!socket_init())
        throw std::runtime_error("failed to initialize TCP socket subsystem");
    socketInitialized_ = true;

    try {
        listenerFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenerFd_ == INVALID_SOCKET_FD) {
            throw std::runtime_error(std::string("failed to create TCP listener: ") + socket_strerror());
        }

        const int enabled = 1;
        if (setsockopt(listenerFd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) < 0) {
            throw std::runtime_error(std::string("failed to configure TCP listener: ") + socket_strerror());
        }
        if (!socket_set_nonblocking(listenerFd_)) {
            throw std::runtime_error(std::string("failed to make TCP listener nonblocking: ") + socket_strerror());
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(config_.port));
        if (inet_pton(AF_INET, config_.bindAddress.c_str(), &address.sin_addr) != 1) {
            throw std::runtime_error("invalid TCP bind address: " + config_.bindAddress);
        }
        if (bind(listenerFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            throw std::runtime_error(std::string("failed to bind TCP listener: ") + socket_strerror());
        }
        if (listen(listenerFd_, static_cast<int>(config_.maxClients)) < 0) {
            throw std::runtime_error(std::string("failed to listen for TCP clients: ") + socket_strerror());
        }
    } catch (...) {
        stop();
        throw;
    }
}

void TcpServer::Impl::poll()
{
    if (listenerFd_ == INVALID_SOCKET_FD)
        throw std::logic_error("TCP server is not running");
    expireIdleClients();
    closeRequestedClients();

    std::vector<pollfd> descriptors;
    std::vector<PolledClient> clients;
    descriptors.push_back({listenerFd_, POLLIN, 0});
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        clients.reserve(connections_.size());
        descriptors.reserve(1 + connections_.size());
        for (const auto& connection : connections_) {
            short events = POLLIN;
            if (!connection.outboundQueue.empty())
                events |= POLLOUT;
            descriptors.push_back({connection.fd, events, 0});
            clients.push_back({connection.id, connection.fd});
        }
    }

    const int ready = socket_poll(descriptors.data(), descriptors.size(), 0);
    if (ready < 0) {
        if (socket_interrupted())
            return;
        throw std::runtime_error(std::string("TCP poll failed: ") + socket_strerror());
    }
    if ((descriptors.front().revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        throw std::runtime_error("TCP listener failed");
    }
    if ((descriptors.front().revents & POLLIN) != 0)
        acceptClients();

    for (std::size_t index = 0; index < clients.size(); ++index) {
        const auto& client = clients[index];
        const short events = descriptors[index + 1].revents;
        if (!isCurrentClient(client))
            continue;
        if ((events & POLLIN) != 0)
            readClient(client);
        if (isCurrentClient(client) && (events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            closeClient(client.id, "connection closed");
        }
        if (isCurrentClient(client) && (events & POLLOUT) != 0)
            drainOutbound(client);
    }

    // A receive callback may enqueue a response during this poll cycle.
    drainAllOutbound();
    closeRequestedClients();
}

void TcpServer::Impl::stop() noexcept
{
    std::vector<ClientId> clientIds;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        clientIds.reserve(connections_.size());
        for (const auto& connection : connections_)
            clientIds.push_back(connection.id);
    }
    for (const ClientId clientId : clientIds)
        closeClient(clientId, "server shutdown");

    if (listenerFd_ != INVALID_SOCKET_FD) {
        socket_close(listenerFd_);
        listenerFd_ = INVALID_SOCKET_FD;
    }
    if (socketInitialized_) {
        socket_cleanup();
        socketInitialized_ = false;
    }
}

bool TcpServer::Impl::send(ClientId clientId, const uint8_t* data, std::size_t size)
{
    if (clientId == 0 || data == nullptr || size == 0)
        return false;
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    auto connection =
        std::find_if(connections_.begin(), connections_.end(), [clientId](const auto& candidate) { return candidate.id == clientId; });
    if (connection == connections_.end() || connection->closeRequested)
        return false;
    if (size > config_.transmitBufferBytes || connection->queuedBytes > config_.transmitBufferBytes - size) {
        connection->closeRequested = true;
        connection->closeReason = "transmit buffer overflow";
        return false;
    }
    connection->outboundQueue.emplace_back(data, data + size);
    connection->queuedBytes += size;
    return true;
}

void TcpServer::Impl::disconnect(ClientId clientId, std::string reason)
{
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    auto connection =
        std::find_if(connections_.begin(), connections_.end(), [clientId](const auto& candidate) { return candidate.id == clientId; });
    if (connection == connections_.end())
        return;
    connection->closeRequested = true;
    if (connection->closeReason.empty())
        connection->closeReason = std::move(reason);
}

bool TcpServer::Impl::hasClient(ClientId clientId) const
{
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    return std::any_of(connections_.begin(), connections_.end(),
                       [clientId](const auto& connection) { return connection.id == clientId && !connection.closeRequested; });
}

std::size_t TcpServer::Impl::clientCount() const
{
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    return connections_.size();
}

std::size_t TcpServer::Impl::queuedBytes(ClientId clientId) const
{
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    const auto connection =
        std::find_if(connections_.begin(), connections_.end(), [clientId](const auto& candidate) { return candidate.id == clientId; });
    return connection == connections_.end() ? 0 : connection->queuedBytes;
}

void TcpServer::Impl::acceptClients()
{
    while (true) {
        sockaddr_in peer{};
        socklen_t peerLength = sizeof(peer);
        const socket_t accepted = accept(listenerFd_, reinterpret_cast<sockaddr*>(&peer), &peerLength);
        if (accepted == INVALID_SOCKET_FD) {
            if (socket_would_block())
                return;
            if (socket_interrupted())
                continue;
            throw std::runtime_error(std::string("failed to accept TCP client: ") + socket_strerror());
        }

        bool full = false;
        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            full = connections_.size() >= config_.maxClients;
        }
        if (full) {
            socket_close(accepted);
            if (rejectedHandler_)
                rejectedHandler_("maximum clients reached");
            continue;
        }
        if (!configureClient(accepted)) {
            const std::string reason = std::string("client configuration failed: ") + socket_strerror();
            socket_close(accepted);
            if (rejectedHandler_)
                rejectedHandler_(reason);
            continue;
        }

        ClientId clientId;
        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            clientId = nextClientId_++;
            Connection connection;
            connection.id = clientId;
            connection.fd = accepted;
            connection.lastReceive = std::chrono::steady_clock::now();
            connections_.push_back(std::move(connection));
        }
        if (connectedHandler_)
            connectedHandler_(clientId);
    }
}

bool TcpServer::Impl::configureClient(socket_t clientFd) const
{
    if (!socket_set_nonblocking(clientFd))
        return false;
    const int enabled = 1;
    if (config_.tcpNoDelay &&
        setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) < 0) {
        return false;
    }
    if (config_.keepAlive && setsockopt(clientFd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) < 0) {
        return false;
    }
#if defined(__APPLE__) && defined(SO_NOSIGPIPE)
    if (setsockopt(clientFd, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) < 0) {
        return false;
    }
#endif
#if defined(__linux__) && defined(TCP_USER_TIMEOUT)
    if (config_.tcpUserTimeout.count() > 0) {
        const int timeout = static_cast<int>(config_.tcpUserTimeout.count());
        if (setsockopt(clientFd, IPPROTO_TCP, TCP_USER_TIMEOUT, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) < 0) {
            return false;
        }
    }
#endif
    return true;
}

void TcpServer::Impl::readClient(const PolledClient& client)
{
    while (isCurrentClient(client)) {
        const ssize_t received = socket_read(client.fd, receiveBuffer_.data(), receiveBuffer_.size());
        if (received > 0) {
            {
                std::lock_guard<std::mutex> lock(connectionsMutex_);
                auto connection = std::find_if(connections_.begin(), connections_.end(), [&client](const auto& candidate) {
                    return candidate.id == client.id && candidate.fd == client.fd;
                });
                if (connection == connections_.end())
                    return;
                connection->lastReceive = std::chrono::steady_clock::now();
            }
            if (dataHandler_) {
                dataHandler_(client.id, receiveBuffer_.data(), static_cast<std::size_t>(received));
            }
            if (isCloseRequested(client.id))
                return;
            continue;
        }
        if (received == 0) {
            closeClient(client.id, "peer closed connection");
            return;
        }
        if (socket_would_block())
            return;
        if (socket_interrupted())
            continue;
        closeClient(client.id, std::string("receive error: ") + socket_strerror());
        return;
    }
}

void TcpServer::Impl::drainOutbound(const PolledClient& client)
{
    while (true) {
        std::unique_lock<std::mutex> lock(connectionsMutex_);
        auto connection = std::find_if(connections_.begin(), connections_.end(),
                                       [&client](const auto& candidate) { return candidate.id == client.id && candidate.fd == client.fd; });
        if (connection == connections_.end() || connection->closeRequested || connection->outboundQueue.empty())
            return;

        auto& message = connection->outboundQueue.front();
        const uint8_t* data = message.data() + connection->sendOffset;
        const std::size_t remaining = message.size() - connection->sendOffset;
        const ssize_t sent = socket_write(connection->fd, data, remaining);
        if (sent > 0) {
            const auto count = static_cast<std::size_t>(sent);
            connection->sendOffset += count;
            connection->queuedBytes -= count;
            if (connection->sendOffset == message.size()) {
                connection->outboundQueue.pop_front();
                connection->sendOffset = 0;
            }
            continue;
        }
        if (sent < 0 && socket_would_block())
            return;
        if (sent < 0 && socket_interrupted())
            continue;
        lock.unlock();
        closeClient(client.id, sent == 0 ? "send returned zero" : std::string("send error: ") + socket_strerror());
        return;
    }
}

void TcpServer::Impl::drainAllOutbound()
{
    std::vector<PolledClient> clients;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        for (const auto& connection : connections_) {
            if (!connection.outboundQueue.empty())
                clients.push_back({connection.id, connection.fd});
        }
    }
    for (const auto& client : clients)
        drainOutbound(client);
}

void TcpServer::Impl::expireIdleClients()
{
    if (config_.idleTimeout.count() <= 0)
        return;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    for (auto& connection : connections_) {
        if (now - connection.lastReceive >= config_.idleTimeout) {
            connection.closeRequested = true;
            if (connection.closeReason.empty())
                connection.closeReason = "client timeout";
        }
    }
}

void TcpServer::Impl::closeRequestedClients()
{
    while (true) {
        ClientId clientId = 0;
        std::string reason;
        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            const auto connection =
                std::find_if(connections_.begin(), connections_.end(), [](const auto& candidate) { return candidate.closeRequested; });
            if (connection == connections_.end())
                return;
            clientId = connection->id;
            reason = connection->closeReason;
        }
        closeClient(clientId, reason.empty() ? "transport close requested" : reason);
    }
}

void TcpServer::Impl::closeClient(ClientId clientId, const std::string& reason)
{
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        const auto connection =
            std::find_if(connections_.begin(), connections_.end(), [clientId](const auto& candidate) { return candidate.id == clientId; });
        if (connection == connections_.end())
            return;
        socket_close(connection->fd);
        connections_.erase(connection);
    }
    if (disconnectedHandler_) {
        try {
            disconnectedHandler_(clientId, reason);
        } catch (...) {
            // Socket cleanup must complete even if an observer fails.
        }
    }
}

bool TcpServer::Impl::isCurrentClient(const PolledClient& client) const
{
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    return std::any_of(connections_.begin(), connections_.end(),
                       [&client](const auto& connection) { return connection.id == client.id && connection.fd == client.fd; });
}

bool TcpServer::Impl::isCloseRequested(ClientId clientId) const
{
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    const auto connection =
        std::find_if(connections_.begin(), connections_.end(), [clientId](const auto& candidate) { return candidate.id == clientId; });
    return connection == connections_.end() || connection->closeRequested;
}

TcpServer::TcpServer(Config config) : impl_(std::make_unique<Impl>(std::move(config)))
{
}

TcpServer::~TcpServer() = default;

void TcpServer::setPort(int port)
{
    impl_->setPort(port);
}

void TcpServer::setConnectedHandler(ConnectedHandler handler)
{
    impl_->setConnectedHandler(std::move(handler));
}

void TcpServer::setDataHandler(DataHandler handler)
{
    impl_->setDataHandler(std::move(handler));
}

void TcpServer::setDisconnectedHandler(DisconnectedHandler handler)
{
    impl_->setDisconnectedHandler(std::move(handler));
}

void TcpServer::setRejectedHandler(RejectedHandler handler)
{
    impl_->setRejectedHandler(std::move(handler));
}

void TcpServer::start()
{
    impl_->start();
}

void TcpServer::poll()
{
    impl_->poll();
}

void TcpServer::stop() noexcept
{
    impl_->stop();
}

bool TcpServer::send(ClientId clientId, const uint8_t* data, std::size_t size)
{
    return impl_->send(clientId, data, size);
}

void TcpServer::disconnect(ClientId clientId, std::string reason)
{
    impl_->disconnect(clientId, std::move(reason));
}

bool TcpServer::hasClient(ClientId clientId) const
{
    return impl_->hasClient(clientId);
}

std::size_t TcpServer::clientCount() const
{
    return impl_->clientCount();
}

std::size_t TcpServer::queuedBytes(ClientId clientId) const
{
    return impl_->queuedBytes(clientId);
}
