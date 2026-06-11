#include "SocketWrapper.hpp"
#include "common/config.hpp"

#include <algorithm>
#include <cstring>
#include <thread>
#include <chrono>

SocketWrapper::DataHistorianServer::DataHistorianServer(std::chrono::milliseconds pollPeriod) : PeriodicTask(pollPeriod) {
    std::fill(std::begin(tcpServer_.client_fds), std::end(tcpServer_.client_fds), INVALID_SOCKET_FD);
    std::fill(std::begin(lastPacketTime_), std::end(lastPacketTime_), std::chrono::steady_clock::now());
}

void SocketWrapper::DataHistorianServer::setPort(int port) { port_ = port; }
void SocketWrapper::DataHistorianServer::setCallback(DataHistorianCallback cb) { callback_ = std::move(cb); }
TcpSocketStatus SocketWrapper::DataHistorianServer::status() const { return status_.load(); }

void SocketWrapper::DataHistorianServer::onStart() {
    if (!socket_init()) {
        SOCKET_DH_ERR("Failed to initialize socket subsystem: " << socket_strerror());
        status_.store(tcpSOCKET_ERROR);
        return;
    }

    std::fill(std::begin(tcpServer_.client_fds), std::end(tcpServer_.client_fds), INVALID_SOCKET_FD);

    tcpServer_.server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcpServer_.server_fd == INVALID_SOCKET_FD) {
        SOCKET_DH_ERR("Failed to create TCP socket: " << socket_strerror());
        status_.store(tcpSOCKET_ERROR);
        socket_cleanup();
        return;
    }

    if (setsockopt(tcpServer_.server_fd, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&tcpServer_.opt), sizeof(tcpServer_.opt)) < 0) {
        SOCKET_DH_ERR("Failed to set SO_REUSEADDR on TCP socket: " << socket_strerror());
        status_.store(tcpSOCKET_ERROR);
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
        status_.store(tcpSOCKET_ERROR);
        socket_close(tcpServer_.server_fd);
        tcpServer_.server_fd = INVALID_SOCKET_FD;
        socket_cleanup();
        return;
    }

    if (!socket_set_nonblocking(tcpServer_.server_fd)) {
        SOCKET_DH_ERR("Failed to set non-blocking mode on server socket: " << socket_strerror());
        status_.store(tcpSOCKET_ERROR);
        socket_close(tcpServer_.server_fd);
        tcpServer_.server_fd = INVALID_SOCKET_FD;
        socket_cleanup();
        return;
    }

    // Start listening for incoming connections
    if (listen(tcpServer_.server_fd, TCP_MAX_CONNECTIONS) < 0) {
        SOCKET_DH_ERR("Failed to listen on TCP socket: " << socket_strerror());
        status_.store(tcpSOCKET_ERROR);
        socket_close(tcpServer_.server_fd);
        tcpServer_.server_fd = INVALID_SOCKET_FD;
        socket_cleanup();
        return;
    }

    status_.store(tcpSOCKET_CONNECTED);
    SOCKET_DH_ST("Data historian server listening on port " << port_);
}

void SocketWrapper::DataHistorianServer::execute() {
    if (status_.load() < tcpSOCKET_CONNECTED) return;

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
        status_.store(tcpSOCKET_ERROR);
        return;
    }

    // Check for client timeouts based on last packet received.
    const auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < TCP_MAX_CONNECTIONS; ++i) {
        socket_t fd = tcpServer_.client_fds[i];
        if (fd == INVALID_SOCKET_FD) continue;

        if (now - lastPacketTime_[i] >= clientTimeoutInterval_) {
            SOCKET_DH_LOG_V2("Client timeout detected, disconnecting client");
            removeClient(fd);
        }
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

void SocketWrapper::DataHistorianServer::acceptNewClients() {
    while (true) {
        socket_t clientFd = accept(
            tcpServer_.server_fd,
            reinterpret_cast<struct sockaddr*>(&tcpServer_.address),
            &tcpServer_.addrlen);

        if (clientFd == INVALID_SOCKET_FD) {
            if (socket_would_block()) break;
            SOCKET_DH_ERR("accept failed: " << socket_strerror());
            status_.store(tcpSOCKET_ERROR);
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

        int slot_index = std::distance(std::begin(tcpServer_.client_fds), free_slot);
        *free_slot = clientFd;
        lastPacketTime_[slot_index] = std::chrono::steady_clock::now();
        SOCKET_DH_ST("Accepted data historian client");
    }
}

void SocketWrapper::DataHistorianServer::readFromClient(socket_t fd) {
    const ssize_t bytes_read = socket_read(fd, tcpServer_.buffer, sizeof(tcpServer_.buffer));

    if (bytes_read > 0) {
        // Update last packet received time.
        socket_t* slot = std::find(
            std::begin(tcpServer_.client_fds),
            std::end(tcpServer_.client_fds),
            fd);
        if (slot != std::end(tcpServer_.client_fds)) {
            int slot_index = std::distance(std::begin(tcpServer_.client_fds), slot);
            lastPacketTime_[slot_index] = std::chrono::steady_clock::now();
        }

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

void SocketWrapper::DataHistorianServer::removeClient(socket_t client_fd) {
    socket_t* slot = std::find(
        std::begin(tcpServer_.client_fds),
        std::end(tcpServer_.client_fds),
        client_fd);

    if (slot != std::end(tcpServer_.client_fds)) {
        socket_close(client_fd);
        *slot = INVALID_SOCKET_FD;
        SOCKET_DH_ST("Data historian client disconnected");
    }
}

void SocketWrapper::DataHistorianServer::onStop() {
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
    status_.store(tcpSOCKET_CLOSED);
    SOCKET_DH_ST("DataHistorian Socket Stopped");
}
