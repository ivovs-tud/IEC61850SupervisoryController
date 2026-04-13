#include "SocketWrapper.hpp"
#include "common/config.hpp"

#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// OperatorServer
// ---------------------------------------------------------------------------

SocketWrapper::OperatorServer::OperatorServer(std::chrono::milliseconds pollPeriod)
    : PeriodicTask(pollPeriod) {}

void SocketWrapper::OperatorServer::setPort(int port)             { port_ = port; }
void SocketWrapper::OperatorServer::setCallback(OperatorCallback cb) { callback_ = std::move(cb); }
SocketStatus SocketWrapper::OperatorServer::status() const        { return status_.load(); }

void SocketWrapper::OperatorServer::onStart()
{
    try {
        socket_.emplace(context_, zmq::socket_type::pair);
        socket_->set(zmq::sockopt::rcvhwm, 3);
        socket_->bind("tcp://*:" + std::to_string(port_));
        SOCKET_OP_LOG_V1("Operator server listening on port " << port_);
        status_.store(SOCKET_CONNECTED);
    } catch (const std::exception& e) {
        SOCKET_OP_ERR("Failed to start operator server on port " << port_ << ": " << e.what());
        status_.store(SOCKET_ERROR);
    }
}

void SocketWrapper::OperatorServer::execute()
{
    if (!socket_ || status_.load() < SOCKET_CONNECTED) return;

    zmq::message_t message;
    const auto result = socket_->recv(message, zmq::recv_flags::dontwait);
    if (!result) return;  // no message this cycle

    SOCKET_OP_LOG_V2("Received a message of size " << message.size() << " bytes");

    if (callback_) {
        try {
            std::vector<float> data;
            auto handle = msgpack::unpack(static_cast<const char*>(message.data()), message.size());
            handle.get().convert(data);
            SOCKET_OP_LOG_V2("Unpacked vector of size " << data.size());
            callback_(data);
        } catch (const std::exception& e) {
            SOCKET_OP_ERR("Failed to unpack message: " << e.what());
        }
    }
}

void SocketWrapper::OperatorServer::onStop()
{
    socket_.reset();  // close socket before context is destroyed
    status_.store(SOCKET_CLOSED);
    SOCKET_OP_LOG_V1("Operator server stopped on port " << port_);
}

// ---------------------------------------------------------------------------
// AttackInterfaceServer
// ---------------------------------------------------------------------------

SocketWrapper::AttackInterfaceServer::AttackInterfaceServer(std::chrono::milliseconds pollPeriod)
    : PeriodicTask(pollPeriod) {}

void SocketWrapper::AttackInterfaceServer::setPort(int port) { port_ = port; }
SocketStatus SocketWrapper::AttackInterfaceServer::status() const { return status_.load(); }
void SocketWrapper::AttackInterfaceServer::setCallback(AttackCallback cb) { callback_ = std::move(cb); }

void SocketWrapper::AttackInterfaceServer::onStart()
{
    try {
        socket_.emplace(context_, zmq::socket_type::pair);
        socket_->set(zmq::sockopt::rcvhwm, 3);
        socket_->bind("tcp://*:" + std::to_string(port_));
        SOCKET_AT_LOG_V1("Attack interface server listening on port " << port_);
        status_.store(SOCKET_CONNECTED);
    } catch (const std::exception& e) {
        SOCKET_AT_ERR("Failed to start attack interface server on port " << port_ << ": " << e.what());
        status_.store(SOCKET_ERROR);
    }
}

void SocketWrapper::AttackInterfaceServer::execute()
{
    if (!socket_ || status_.load() < SOCKET_CONNECTED) return;

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
    // TODO: parse and handle attack / injection commands
}

void SocketWrapper::AttackInterfaceServer::onStop()
{
    socket_.reset();
    status_.store(SOCKET_CLOSED);
    SOCKET_AT_LOG_V1("Attack interface server stopped on port " << port_);
}

// ---------------------------------------------------------------------------
// DataHistorianServer
// ---------------------------------------------------------------------------

SocketWrapper::DataHistorianServer::DataHistorianServer(std::chrono::milliseconds pollPeriod)
    : PeriodicTask(pollPeriod) {
        std::fill(std::begin(tcpServer_.client_fds), std::end(tcpServer_.client_fds), INVALID_SOCKET_FD);
    }

void SocketWrapper::DataHistorianServer::setPort(int port) { port_ = port; }
void SocketWrapper::DataHistorianServer::setCallback(DataHistorianCallback cb) { callback_ = std::move(cb); }
SocketStatus SocketWrapper::DataHistorianServer::status() const { return status_.load(); }

void SocketWrapper::DataHistorianServer::onStart()
{
    if (!socket_init()) {
        SOCKET_DH_ERR("Failed to initialize socket subsystem: " << socket_strerror());
        status_.store(SOCKET_ERROR);
        return;
    }

    std::fill(std::begin(tcpServer_.client_fds), std::end(tcpServer_.client_fds), INVALID_SOCKET_FD);

    tcpServer_.server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcpServer_.server_fd == INVALID_SOCKET_FD) {
        SOCKET_DH_ERR("Failed to create TCP socket: " << socket_strerror());
        status_.store(SOCKET_ERROR);
        socket_cleanup();
        return;
    }

    if (setsockopt(tcpServer_.server_fd, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&tcpServer_.opt), sizeof(tcpServer_.opt)) < 0) {
        SOCKET_DH_ERR("Failed to set SO_REUSEADDR on TCP socket: " << socket_strerror());
        status_.store(SOCKET_ERROR);
        socket_close(tcpServer_.server_fd);
        tcpServer_.server_fd = INVALID_SOCKET_FD;
        socket_cleanup();
        return;
    }

    std::memset(&tcpServer_.address, 0, sizeof(tcpServer_.address));
    tcpServer_.address.sin_family = AF_INET;
    tcpServer_.address.sin_addr.s_addr = INADDR_ANY;
    tcpServer_.address.sin_port = htons(static_cast<uint16_t>(port_));

    if (bind(tcpServer_.server_fd, reinterpret_cast<struct sockaddr*>(&tcpServer_.address), sizeof(tcpServer_.address)) < 0) {
        SOCKET_DH_ERR("Failed to bind TCP socket to port " << port_ << ": " << socket_strerror());
        status_.store(SOCKET_ERROR);
        socket_close(tcpServer_.server_fd);
        tcpServer_.server_fd = INVALID_SOCKET_FD;
        socket_cleanup();
        return;
    }

    if (!socket_set_nonblocking(tcpServer_.server_fd)) {
        SOCKET_DH_ERR("Failed to set non-blocking mode on server socket: " << socket_strerror());
        status_.store(SOCKET_ERROR);
        socket_close(tcpServer_.server_fd);
        tcpServer_.server_fd = INVALID_SOCKET_FD;
        socket_cleanup();
        return;
    }

    // Start listening for incoming connections
    if (listen(tcpServer_.server_fd, TCP_MAX_CONNECTIONS) < 0) {
        SOCKET_DH_ERR("Failed to listen on TCP socket: " << socket_strerror());
        status_.store(SOCKET_ERROR);
        socket_close(tcpServer_.server_fd);
        tcpServer_.server_fd = INVALID_SOCKET_FD;
        socket_cleanup();
        return;
    }

    status_.store(SOCKET_CONNECTED);
    SOCKET_DH_LOG_V1("Data historian server listening on port " << port_);
}

void SocketWrapper::DataHistorianServer::execute()
{
    if (status_.load() < SOCKET_CONNECTED) return;

    pollfd pfds[1 + TCP_MAX_CONNECTIONS];
    int nPfds = 0;

    pfds[nPfds].fd = tcpServer_.server_fd;
    pfds[nPfds].events = POLLIN;
    pfds[nPfds].revents = 0;
    ++nPfds;

    for (socket_t fd : tcpServer_.client_fds) {
        if (fd != INVALID_SOCKET_FD) {
            pfds[nPfds].fd = fd;
            pfds[nPfds].events = POLLIN;
            pfds[nPfds].revents = 0;
            ++nPfds;
        }
    }

    const int ready = socket_poll(pfds, static_cast<std::size_t>(nPfds), 0);
    if (ready < 0) {
        SOCKET_DH_ERR("poll failed: " << socket_strerror());
        status_.store(SOCKET_ERROR);
        return;
    }
    if (ready == 0) return;

    if ((pfds[0].revents & POLLIN) != 0) {
        acceptNewClients();
    }

    for (int i = 1; i < nPfds; ++i) {
        if ((pfds[i].revents & POLLIN) != 0) {
            readFromClient(pfds[i].fd);
        }
    }

}

void SocketWrapper::DataHistorianServer::acceptNewClients()
{
    while (true) {
        socket_t clientFd = accept(
            tcpServer_.server_fd,
            reinterpret_cast<struct sockaddr*>(&tcpServer_.address),
            &tcpServer_.addrlen);

        if (clientFd == INVALID_SOCKET_FD) {
            if (socket_would_block()) break;
            SOCKET_DH_ERR("accept failed: " << socket_strerror());
            status_.store(SOCKET_ERROR);
            break;
        }

        if (!socket_set_nonblocking(clientFd)) {
            SOCKET_DH_ERR("Failed to set non-blocking mode on client socket: " << socket_strerror());
            socket_close(clientFd);
            continue;
        }

        socket_t* free_slot = std::find(
            std::begin(tcpServer_.client_fds),
            std::end(tcpServer_.client_fds),
            INVALID_SOCKET_FD);

        if (free_slot == std::end(tcpServer_.client_fds)) {
            SOCKET_DH_ERR("Rejecting client: maximum number of connections reached");
            socket_close(clientFd);
            continue;
        }

        *free_slot = clientFd;
        SOCKET_DH_LOG_V2("Accepted data historian client");
    }
}

void SocketWrapper::DataHistorianServer::readFromClient(socket_t fd)
{
    const ssize_t bytes_read = socket_read(fd, tcpServer_.buffer, sizeof(tcpServer_.buffer));

    if (bytes_read > 0) {
        if (callback_) {
            callback_(reinterpret_cast<const uint8_t*>(tcpServer_.buffer), static_cast<size_t>(bytes_read));
        }
        return;
    }

    if (bytes_read == 0) {
        removeClient(fd);
        return;
    }

    if (!socket_would_block()) {
        SOCKET_DH_ERR("Read failed on client socket: " << socket_strerror());
        removeClient(fd);
    }
}

void SocketWrapper::DataHistorianServer::removeClient(socket_t client_fd)
{
    socket_t* slot = std::find(
        std::begin(tcpServer_.client_fds),
        std::end(tcpServer_.client_fds),
        client_fd);

    if (slot != std::end(tcpServer_.client_fds)) {
        socket_close(client_fd);
        *slot = INVALID_SOCKET_FD;
        SOCKET_DH_LOG_V2("Data historian client disconnected");
    }
}

void SocketWrapper::DataHistorianServer::onStop()
{
    for (socket_t& fd : tcpServer_.client_fds) {
        if (fd != INVALID_SOCKET_FD) {
            socket_close(fd);
            fd = INVALID_SOCKET_FD;
        }
    }

    std::fill(std::begin(tcpServer_.client_fds), std::end(tcpServer_.client_fds), INVALID_SOCKET_FD);

    if (tcpServer_.server_fd != INVALID_SOCKET_FD) {
        socket_close(tcpServer_.server_fd);
        tcpServer_.server_fd = INVALID_SOCKET_FD;
    }

    socket_cleanup();
    status_.store(SOCKET_CLOSED);
}

// ---------------------------------------------------------------------------
// SocketWrapper (public API)
// ---------------------------------------------------------------------------

SocketWrapper::SocketWrapper()
    : lastActivityTime_(std::chrono::system_clock::now()) {}

SocketStatus SocketWrapper::StartOperatorServer(int port)
{
    if (opServer_.status() >= SOCKET_CONNECTED) {
        SOCKET_OP_ERR("Operator server is already running.");
        return SOCKET_ERROR;
    }
    if (port < 1024 || port > 65535) {
        SOCKET_OP_ERR("Invalid port number: " << port);
        return SOCKET_ERROR;
    }
    opServer_.setPort(port);
    opServer_.start();
    return opServer_.status();
}

SocketStatus SocketWrapper::StopOperatorServer()
{
    opServer_.stop();
    return opServer_.status();
}

void SocketWrapper::AttachServerCallback(OperatorCallback callback)
{
    opServer_.setCallback(std::move(callback));
}

void SocketWrapper::AttachAttackInterfaceCallback(AttackCallback callback)
{
    attackServer_.setCallback(std::move(callback));
}

SocketStatus SocketWrapper::StartAttackInterfaceServer(int port)
{
    if (attackServer_.status() >= SOCKET_CONNECTED) {
        SOCKET_AT_ERR("Attack interface server is already running.");
        return SOCKET_ERROR;
    }
    if (port < 1024 || port > 65535) {
        SOCKET_AT_ERR("Invalid port number: " << port);
        return SOCKET_ERROR;
    }
    attackServer_.setPort(port);
    attackServer_.start();
    return attackServer_.status();
}

SocketStatus SocketWrapper::StopAttackInterfaceServer()
{
    attackServer_.stop();
    return attackServer_.status();
}

void SocketWrapper::AttackInterfaceServer::txData(const std::shared_ptr<void>&data, size_t dataSize) {
    zmq::message_t message(dataSize);
    std::memcpy(message.data(), data.get(), dataSize);
    socket_->send(message, zmq::send_flags::dontwait);
}

void  SocketWrapper::txAttackInterfaceData(const std::shared_ptr<void>&data, size_t dataSize) {
    attackServer_.txData(data, dataSize);
}

void SocketWrapper::AttachDataHistorianCallback(DataHistorianCallback callback)
{
    dataHistorianServer_.setCallback(std::move(callback));
}

SocketStatus SocketWrapper::StartDataHistorianServer(int port)
{
    if (dataHistorianServer_.status() >= SOCKET_CONNECTED) {
        SOCKET_DH_ERR("Data historian server is already running.");
        return SOCKET_ERROR;
    }
    if (port < 1024 || port > 65535) {
        SOCKET_DH_ERR("Invalid port number: " << port);
        return SOCKET_ERROR;
    }

    dataHistorianServer_.setPort(port);
    dataHistorianServer_.start();
    return dataHistorianServer_.status();
}

SocketStatus SocketWrapper::StopDataHistorianServer()
{
    dataHistorianServer_.stop();
    return dataHistorianServer_.status();
}
