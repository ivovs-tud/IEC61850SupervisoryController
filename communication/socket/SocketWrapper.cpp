#include "SocketWrapper.hpp"
#include "common/config.hpp"

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