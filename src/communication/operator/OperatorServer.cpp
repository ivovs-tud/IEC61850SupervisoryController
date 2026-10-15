#include "OperatorServer.hpp"

#include "sc/runtime/Logging.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {

OperatorCommand decodeOperatorCommand(const uint8_t* data, std::size_t size)
{
    if (size == sizeof(float)) {
        float value = 0.0F;
        std::memcpy(&value, data, sizeof(value));
        if (!std::isfinite(value)) {
            throw std::runtime_error("requested power must be finite");
        }
        return RequestedPowerCommand{value};
    }

    if (size >= sizeof(uint32_t) + sizeof(uint8_t)) {
        uint32_t marker = 0;
        std::memcpy(&marker, data, sizeof(marker));
        if (marker == 0x01010101) {
            return SimulationStateCommand{data[sizeof(marker)] != 0};
        }
    }

    throw std::runtime_error("operator message has an unexpected format or size");
}

} // namespace

OperatorServer::OperatorServer(Config config) : PeriodicTask(config.pollPeriod), config_(config)
{
    if (config_.port < 1024 || config_.port > 65535) {
        throw std::invalid_argument("invalid operator server port");
    }
}

OperatorServer::~OperatorServer()
{
    stop();
}

void OperatorServer::setCommandHandler(CommandHandler handler)
{
    commandHandler_ = std::move(handler);
}

void OperatorServer::onStart()
{
    socket_.emplace(context_, zmq::socket_type::pair);
    socket_->set(zmq::sockopt::rcvhwm, 3);
    socket_->bind("tcp://*:" + std::to_string(config_.port));
    SOCKET_OP_ST("Operator server listening on port " << config_.port);
}

void OperatorServer::execute()
{
    if (!socket_)
        return;

    zmq::message_t message;
    if (!socket_->recv(message, zmq::recv_flags::dontwait))
        return;
    SOCKET_OP_LOG_V2("Received a message of size " << message.size() << " bytes");

    try {
        const auto command = decodeOperatorCommand(static_cast<const uint8_t*>(message.data()), message.size());
        if (commandHandler_)
            commandHandler_(command);
    } catch (const std::exception& error) {
        SOCKET_OP_ERR("Rejected operator message: " << error.what());
    }
}

void OperatorServer::onStop()
{
    socket_.reset();
    SOCKET_OP_ST("Operator server stopped on port " << config_.port);
}
