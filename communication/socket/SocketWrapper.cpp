#include "SocketWrapper.hpp"
#include "common/config.hpp"

#include <algorithm>
#include <cstring>
SocketWrapper::SocketWrapper() : lastActivityTime_(std::chrono::system_clock::now()) {}

tcpSocketStatus SocketWrapper::StartOperatorServer(int port) {
    if (opServer_.status() >= tcpSOCKET_CONNECTED) {
        SOCKET_OP_ERR("Operator server is already running.");
        return tcpSOCKET_ERROR;
    }
    if (port < 1024 || port > 65535) {
        SOCKET_OP_ERR("Invalid port number: " << port);
        return tcpSOCKET_ERROR;
    }
    opServer_.setPort(port);
    try {
        if (!opServer_.start() || opServer_.status() < tcpSOCKET_CONNECTED) {
            opServer_.stop();
            return tcpSOCKET_ERROR;
        }
    } catch (const std::exception& error) {
        SOCKET_OP_ERR("Failed to start operator server: " << error.what());
        opServer_.stop();
        return tcpSOCKET_ERROR;
    }
    return tcpSOCKET_CONNECTED;
}

tcpSocketStatus SocketWrapper::StopOperatorServer() {
    opServer_.stop();
    return opServer_.status();
}

void SocketWrapper::AttachOpServerCallback(OperatorCallback callback) {
    opServer_.setCallback(std::move(callback));
}

void SocketWrapper::AttachAttackInterfaceCallback(AttackCallback callback) {
    attackServer_.setCallback(std::move(callback));
}

tcpSocketStatus SocketWrapper::StartAttackInterfaceServer(int port) {
    if (attackServer_.status() >= tcpSOCKET_CONNECTED) {
        SOCKET_AT_ERR("Attack interface server is already running.");
        return tcpSOCKET_ERROR;
    }
    if (port < 1024 || port > 65535) {
        SOCKET_AT_ERR("Invalid port number: " << port);
        return tcpSOCKET_ERROR;
    }
    attackServer_.setPort(port);
    try {
        if (!attackServer_.start() || attackServer_.status() < tcpSOCKET_CONNECTED) {
            attackServer_.stop();
            return tcpSOCKET_ERROR;
        }
    } catch (const std::exception& error) {
        SOCKET_AT_ERR("Failed to start attack interface server: " << error.what());
        attackServer_.stop();
        return tcpSOCKET_ERROR;
    }
    return tcpSOCKET_CONNECTED;
}

tcpSocketStatus SocketWrapper::StopAttackInterfaceServer() {
    attackServer_.stop();
    return attackServer_.status();
}

bool SocketWrapper::AttackInterfaceServer::txData(const uint8_t* data, size_t dataSize) {
    if (status_.load() < tcpSOCKET_CONNECTED || data == nullptr || dataSize == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(outboundMutex_);
    if (status_.load() < tcpSOCKET_CONNECTED) {
        return false;
    }
    if (outboundQueue_.size() >= kMaxPendingMessages) {
        SOCKET_AT_ERR("Attack interface outbound queue is full; dropping message");
        return false;
    }
    outboundQueue_.emplace_back(data, data + dataSize);
    return true;
}

void SocketWrapper::txAttackInterfaceData(const std::shared_ptr<void>& data, size_t dataSize) {
    send(static_cast<const uint8_t*>(data.get()), dataSize);
}

void SocketWrapper::setReceiveHandler(sc::ports::AttackReceiveHandler handler) {
    AttachAttackInterfaceCallback(std::move(handler));
}

bool SocketWrapper::send(const uint8_t* data, std::size_t size) {
    return attackServer_.txData(data, size);
}

void SocketWrapper::setFailureHandler(PeriodicTask::FailureHandler handler) {
    opServer_.setFailureHandler([handler](const std::string& message) {
        if (handler) handler("operator server: " + message);
    });
    attackServer_.setFailureHandler([handler](const std::string& message) {
        if (handler) handler("attack interface server: " + message);
    });
    dataHistorianServer_.setFailureHandler([handler](const std::string& message) {
        if (handler) handler("data historian server: " + message);
    });
}

void SocketWrapper::AttachDataHistorianCallback(DataHistorianCallback callback) {
    dataHistorianServer_.setCallback(std::move(callback));
}

tcpSocketStatus SocketWrapper::StartDataHistorianServer(int port) {
    if (dataHistorianServer_.status() >= tcpSOCKET_CONNECTED) {
        SOCKET_DH_ERR("Data historian server is already running.");
        return tcpSOCKET_ERROR;
    }
    if (port < 1024 || port > 65535) {
        SOCKET_DH_ERR("Invalid port number: " << port);
        return tcpSOCKET_ERROR;
    }

    dataHistorianServer_.setPort(port);
    try {
        if (!dataHistorianServer_.start() ||
            dataHistorianServer_.status() < tcpSOCKET_CONNECTED) {
            dataHistorianServer_.stop();
            return tcpSOCKET_ERROR;
        }
    } catch (const std::exception& error) {
        SOCKET_DH_ERR("Failed to start data historian server: " << error.what());
        dataHistorianServer_.stop();
        return tcpSOCKET_ERROR;
    }
    return tcpSOCKET_CONNECTED;
}

tcpSocketStatus SocketWrapper::StopDataHistorianServer() {
    dataHistorianServer_.stop();
    return dataHistorianServer_.status();
}
