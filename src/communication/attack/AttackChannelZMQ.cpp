#include "AttackChannelZMQ.hpp"

#include "sc/runtime/Logging.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

std::size_t largestInboundMessage(std::size_t turbineCount) {
    return std::max(sc::protocol::attack::CFG_DATA_SIZE,
                    sc::protocol::attack::CT_DATA_PREFIX_SIZE + turbineCount);
}

int receiveHighWaterMark(const AttackChannelZMQ::Config& config) {
    const std::size_t frames = config.receiveBufferBytes / largestInboundMessage(config.turbineCount);
    return static_cast<int>(std::min(frames, static_cast<std::size_t>((std::numeric_limits<int>::max)())));
}

} // namespace

AttackChannelZMQ::AttackChannelZMQ(Config config)
    : PeriodicTask(config.pollPeriod),
      config_(config),
      receiveHighWaterMark_(receiveHighWaterMark(config_)) {
    if (config_.port < 1024 || config_.port > 65535) {
        throw std::invalid_argument("invalid ZeroMQ attack channel port");
    }

    if (config_.turbineCount == 0) {
        throw std::invalid_argument("number of turbines must be positive");
    }
    if (receiveHighWaterMark_ == 0) {
        throw std::invalid_argument("attack receive buffer is too small for a complete message");
    }
    if (config_.transmitBufferBytes == 0) {
        throw std::invalid_argument("attack transmit buffer must not be empty");
    }
    if (config_.heartbeatInterval <= std::chrono::milliseconds::zero() ||
        config_.heartbeatTimeout <= config_.heartbeatInterval) {
        throw std::invalid_argument("invalid ZeroMQ heartbeat timing");
    }
}

AttackChannelZMQ::~AttackChannelZMQ() {
    stop();
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
    socket_->set(zmq::sockopt::rcvhwm, receiveHighWaterMark_);
    socket_->set(zmq::sockopt::heartbeat_ivl, static_cast<int>(config_.heartbeatInterval.count()));
    socket_->set(zmq::sockopt::heartbeat_timeout, static_cast<int>(config_.heartbeatTimeout.count()));
    socket_->bind("tcp://*:" + std::to_string(config_.port));
    SOCKET_AT_ST("ZeroMQ attack interface listening on port " << config_.port);
}

void AttackChannelZMQ::execute() {
    if (!socket_) return;
    if (leaseCheckHandler_) leaseCheckHandler_();

    drainInboundQueue();
    drainOutboundQueue();
}

void AttackChannelZMQ::drainInboundQueue() {
    std::size_t receivedBytes = 0;
    for (int receivedMessages = 0; receivedMessages < receiveHighWaterMark_; ++receivedMessages) {
        zmq::message_t message;
        if (!socket_->recv(message, zmq::recv_flags::dontwait)) {
            return;
        }

        if (message.size() > config_.receiveBufferBytes - receivedBytes) {
            SOCKET_AT_ERR("Attack interface receive buffer is full");
            if (disconnectHandler_) {
                disconnectHandler_("receive buffer overflow");
            }
            return;
        }

        receivedBytes += message.size();
        if (receiveHandler_) {
            receiveHandler_(static_cast<const uint8_t*>(message.data()), message.size());
        }
    }
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
