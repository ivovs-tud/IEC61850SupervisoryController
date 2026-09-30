#include "sc/communication/attack/AttackChannelZMQ.hpp"

#include "sc/runtime/Logging.hpp"

#include <cstring>
#include <stdexcept>
#include <utility>

AttackChannelZMQ::AttackChannelZMQ(Config config) : PeriodicTask(config.pollPeriod), config_(config) {
    if (config_.port < 1024 || config_.port > 65535) {
        throw std::invalid_argument("invalid ZeroMQ attack channel port");
    }

    if (config_.receiveBufferBytes == 0 || config_.transmitBufferBytes == 0) {
        throw std::invalid_argument("attack channel buffers must not be empty");
    }
}

void AttackChannelZMQ::setReceiveHandler(sc::ports::AttackReceiveHandler handler) {
    receiveHandler_ = std::move(handler);
}

void AttackChannelZMQ::setLeaseCheckHandler(sc::ports::AttackLeaseCheckHandler handler) {
    leaseCheckHandler_ = std::move(handler);
}

void AttackChannelZMQ::setDisconnectHandler(sc::ports::AttackDisconnectHandler handler) {
    disconnectHandler_ = std::move(handler);
}

bool AttackChannelZMQ::send(const uint8_t* data, std::size_t size) {
    if (!isRunning() || data == nullptr || size == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(outboundMutex_);
    if (!isRunning()) {
        return false;
    }

    if (size > config_.transmitBufferBytes - queuedBytes_) {
        SOCKET_AT_ERR("Attack interface outbound buffer is full");
        outboundQueue_.clear();
        queuedBytes_ = 0;
        if (disconnectHandler_) disconnectHandler_("transmit buffer overflow");
        return false;
    }
    outboundQueue_.emplace_back(data, data + size);
    queuedBytes_ += size;
    return true;
}

void AttackChannelZMQ::onStart() {
    socket_.emplace(context_, zmq::socket_type::pair);
    socket_->set(zmq::sockopt::rcvhwm, 3);
    socket_->set(zmq::sockopt::heartbeat_ivl, static_cast<int>(config_.heartbeatInterval.count()));
    socket_->set(zmq::sockopt::heartbeat_timeout, static_cast<int>(config_.heartbeatTimeout.count()));
    socket_->bind("tcp://*:" + std::to_string(config_.port));
    SOCKET_AT_ST("ZeroMQ attack interface listening on port " << config_.port);
}

void AttackChannelZMQ::execute() {
    if (!socket_) return;
    if (leaseCheckHandler_) leaseCheckHandler_();

    zmq::message_t message;
    const auto received = socket_->recv(message, zmq::recv_flags::dontwait);
    if (received) {
        if (message.size() > config_.receiveBufferBytes) {
            SOCKET_AT_ERR("Attack interface message exceeds receive buffer limit");
            if (disconnectHandler_) {
                disconnectHandler_("receive buffer overflow");
            }
        } else if (receiveHandler_) {
            receiveHandler_(static_cast<const uint8_t*>(message.data()), message.size());
        }
    }

    drainOutboundQueue();
}

void AttackChannelZMQ::drainOutboundQueue() {
    for (std::size_t sent = 0; sent < kMaxSendsPerCycle; ++sent) {
        std::lock_guard<std::mutex> lock(outboundMutex_);
        if (outboundQueue_.empty()) {
            return;
        }

        const auto& pending = outboundQueue_.front();
        zmq::message_t message(pending.size());
        std::memcpy(message.data(), pending.data(), pending.size());
        try {
            if (!socket_->send(message, zmq::send_flags::dontwait)) {
                return;
            }
            queuedBytes_ -= pending.size();
            outboundQueue_.pop_front();
        } catch (const zmq::error_t& error) {
            SOCKET_AT_ERR("Failed to send attack interface message: " << error.what());
            if (disconnectHandler_) {
                disconnectHandler_("transport send failure");
            }
            throw;
        }
    }
}

void AttackChannelZMQ::onStop() {
    {
        std::lock_guard<std::mutex> lock(outboundMutex_);
        outboundQueue_.clear();
        queuedBytes_ = 0;
    }
    socket_.reset();
    SOCKET_AT_ST("ZeroMQ attack interface stopped on port " << config_.port);
}
