#pragma once

#include "sc/communication/attack/AttackChannel.hpp"
#include "sc/runtime/PeriodicTask.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

#include <zmq.hpp>

class AttackChannelZMQ final : public PeriodicTask, public sc::ports::AttackChannel {
public:
    struct Config {
        int port{9002};
        std::chrono::milliseconds pollPeriod{10};
        std::size_t receiveBufferBytes{64 * 1024};
        std::size_t transmitBufferBytes{64 * 1024};
        std::chrono::milliseconds heartbeatInterval{200};
        std::chrono::milliseconds heartbeatTimeout{750};
    };

    explicit AttackChannelZMQ(Config config);
    ~AttackChannelZMQ() override { stop(); }

    void setReceiveHandler(sc::ports::AttackReceiveHandler handler) override;
    void setLeaseCheckHandler(sc::ports::AttackLeaseCheckHandler handler) override;
    void setDisconnectHandler(sc::ports::AttackDisconnectHandler handler) override;
    bool send(const uint8_t* data, std::size_t size) override;

protected:
    void onStart() override;
    void execute() override;
    void onStop() override;

private:
    static constexpr std::size_t kMaxSendsPerCycle = 8;

    void drainOutboundQueue();

    Config config_;
    zmq::context_t context_;
    std::optional<zmq::socket_t> socket_;
    sc::ports::AttackReceiveHandler receiveHandler_;
    sc::ports::AttackLeaseCheckHandler leaseCheckHandler_;
    sc::ports::AttackDisconnectHandler disconnectHandler_;
    std::mutex outboundMutex_;
    std::deque<std::vector<uint8_t>> outboundQueue_;
    std::size_t queuedBytes_{0};
};
