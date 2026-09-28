#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

namespace sc::ports {

class Clock {
public:
    using SteadyTimePoint = std::chrono::steady_clock::time_point;

    virtual ~Clock() = default;

    virtual SteadyTimePoint steadyNow() const = 0;
    virtual uint64_t unixTimeMilliseconds() const = 0;
    virtual void sleepFor(std::chrono::milliseconds duration) = 0;
};

class SystemClock final : public Clock {
public:
    SteadyTimePoint steadyNow() const override {
        return std::chrono::steady_clock::now();
    }

    uint64_t unixTimeMilliseconds() const override {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    void sleepFor(std::chrono::milliseconds duration) override {
        std::this_thread::sleep_for(duration);
    }
};

inline Clock& systemClock() {
    static SystemClock clock;
    return clock;
}

} // namespace sc::ports
