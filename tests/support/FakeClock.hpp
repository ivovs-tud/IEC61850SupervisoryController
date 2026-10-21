#pragma once

#include "sc/runtime/Clock.hpp"

#include <chrono>
#include <cstdint>

class FakeClock final : public sc::ports::Clock {
    public:
    explicit FakeClock(uint64_t unixTimeMilliseconds = 1'000) : unixTimeMilliseconds_(unixTimeMilliseconds)
    {
    }

    SteadyTimePoint steadyNow() const override
    {
        return steadyNow_;
    }

    uint64_t unixTimeMilliseconds() const override
    {
        return unixTimeMilliseconds_;
    }

    void sleepFor(std::chrono::milliseconds duration) override
    {
        steadyNow_ += duration;
        unixTimeMilliseconds_ += static_cast<uint64_t>(duration.count());
    }

    void advance(std::chrono::milliseconds duration)
    {
        sleepFor(duration);
    }

    private:
    SteadyTimePoint steadyNow_{};
    uint64_t unixTimeMilliseconds_;
};
