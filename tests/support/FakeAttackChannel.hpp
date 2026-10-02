#pragma once

#include "sc/communication/attack/AttackChannel.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>

class FakeAttackChannel final : public sc::ports::AttackChannel {
    public:
    using SendObserver = std::function<void(const std::vector<uint8_t>&)>;

    void setReceiveHandler(sc::ports::AttackReceiveHandler handler) override
    {
        receiveHandler_ = std::move(handler);
    }

    void setLeaseCheckHandler(sc::ports::AttackLeaseCheckHandler handler) override
    {
        leaseCheckHandler_ = std::move(handler);
    }

    void setDisconnectHandler(sc::ports::AttackDisconnectHandler handler) override
    {
        disconnectHandler_ = std::move(handler);
    }

    bool send(const uint8_t* data, std::size_t size) override
    {
        if (data == nullptr || size == 0) {
            return false;
        }

        sentMessages_.emplace_back(data, data + size);
        if (sendObserver_) {
            sendObserver_(sentMessages_.back());
        }
        return sendSucceeds_;
    }

    void receive(const std::vector<uint8_t>& message) const
    {
        if (!receiveHandler_) {
            throw std::logic_error("no attack receive handler is installed");
        }
        receiveHandler_(message.data(), message.size());
    }

    void checkLease() const
    {
        if (!leaseCheckHandler_) {
            throw std::logic_error("no attack lease-check handler is installed");
        }
        leaseCheckHandler_();
    }

    void disconnect(const std::string& reason) const
    {
        if (!disconnectHandler_) {
            throw std::logic_error("no attack disconnect handler is installed");
        }
        disconnectHandler_(reason);
    }

    void setSendObserver(SendObserver observer)
    {
        sendObserver_ = std::move(observer);
    }

    void setSendSucceeds(bool succeeds)
    {
        sendSucceeds_ = succeeds;
    }

    void clearSentMessages()
    {
        sentMessages_.clear();
    }

    const std::vector<std::vector<uint8_t>>& sentMessages() const
    {
        return sentMessages_;
    }

    private:
    sc::ports::AttackReceiveHandler receiveHandler_;
    sc::ports::AttackLeaseCheckHandler leaseCheckHandler_;
    sc::ports::AttackDisconnectHandler disconnectHandler_;
    SendObserver sendObserver_;
    std::vector<std::vector<uint8_t>> sentMessages_;
    bool sendSucceeds_{true};
};
