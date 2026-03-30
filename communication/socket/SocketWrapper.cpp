#include "SocketWrapper.hpp"

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
        std::cout << "[OP] Operator server listening on port " << port_ << "\n";
        status_.store(SOCKET_CONNECTED);
    } catch (const std::exception& e) {
        std::cerr << "[OP] Failed to start operator server on port " << port_
                  << ": " << e.what() << "\n";
        status_.store(SOCKET_ERROR);
    }
}

void SocketWrapper::OperatorServer::execute()
{
    if (!socket_ || status_.load() < SOCKET_CONNECTED) return;

    zmq::message_t message;
    const auto result = socket_->recv(message, zmq::recv_flags::dontwait);
    if (!result) return;  // no message this cycle

    std::cout << "[OP] Received a message of size " << message.size() << " bytes\n";

    if (callback_) {
        try {
            std::vector<float> data;
            auto handle = msgpack::unpack(static_cast<const char*>(message.data()), message.size());
            handle.get().convert(data);
            std::cout << "[OP] Unpacked vector of size " << data.size() << "\n";
            callback_(data);
        } catch (const std::exception& e) {
            std::cerr << "[OP] Failed to unpack message: " << e.what() << "\n";
        }
    }
}

void SocketWrapper::OperatorServer::onStop()
{
    socket_.reset();  // close socket before context is destroyed
    status_.store(SOCKET_CLOSED);
    std::cout << "[OP] Operator server stopped on port " << port_ << "\n";
}

// ---------------------------------------------------------------------------
// AttackInterfaceServer
// ---------------------------------------------------------------------------

SocketWrapper::AttackInterfaceServer::AttackInterfaceServer(std::chrono::milliseconds pollPeriod)
    : PeriodicTask(pollPeriod) {}

void SocketWrapper::AttackInterfaceServer::setPort(int port) { port_ = port; }
SocketStatus SocketWrapper::AttackInterfaceServer::status() const { return status_.load(); }

void SocketWrapper::AttackInterfaceServer::onStart()
{
    try {
        socket_.emplace(context_, zmq::socket_type::pair);
        socket_->set(zmq::sockopt::rcvhwm, 3);
        socket_->bind("tcp://*:" + std::to_string(port_));
        std::cout << "[AT] Attack interface server listening on port " << port_ << "\n";
        status_.store(SOCKET_CONNECTED);
    } catch (const std::exception& e) {
        std::cerr << "[AT] Failed to start attack interface server on port " << port_
                  << ": " << e.what() << "\n";
        status_.store(SOCKET_ERROR);
    }
}

void SocketWrapper::AttackInterfaceServer::execute()
{
    if (!socket_ || status_.load() < SOCKET_CONNECTED) return;

    zmq::message_t message;
    const auto result = socket_->recv(message, zmq::recv_flags::dontwait);
    if (!result) return;

    std::cout << "[AT] Received a message of size " << message.size() << " bytes\n";
    // TODO: parse and handle attack / injection commands
}

void SocketWrapper::AttackInterfaceServer::onStop()
{
    socket_.reset();
    status_.store(SOCKET_CLOSED);
    std::cout << "[AT] Attack interface server stopped on port " << port_ << "\n";
}

// ---------------------------------------------------------------------------
// SocketWrapper (public API)
// ---------------------------------------------------------------------------

SocketWrapper::SocketWrapper()
    : lastActivityTime_(std::chrono::system_clock::now()) {}

SocketStatus SocketWrapper::StartOperatorServer(int port)
{
    if (opServer_.status() >= SOCKET_CONNECTED) {
        std::cerr << "[OP] Operator server is already running.\n";
        return SOCKET_ERROR;
    }
    if (port < 1024 || port > 65535) {
        std::cerr << "[OP] Invalid port number: " << port << "\n";
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

SocketStatus SocketWrapper::StartAttackInterfaceServer(int port)
{
    if (attackServer_.status() >= SOCKET_CONNECTED) {
        std::cerr << "[AT] Attack interface server is already running.\n";
        return SOCKET_ERROR;
    }
    if (port < 1024 || port > 65535) {
        std::cerr << "[AT] Invalid port number: " << port << "\n";
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
