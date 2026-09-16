#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace sc::ports {

using AttackReceiveHandler = std::function<void(const uint8_t*, std::size_t)>;

class AttackChannel {
public:
    virtual ~AttackChannel() = default;

    virtual void setReceiveHandler(AttackReceiveHandler handler) = 0;
    // Implementations that defer sending must copy the bytes before returning.
    virtual bool send(const uint8_t* data, std::size_t size) = 0;
};

} // namespace sc::ports
