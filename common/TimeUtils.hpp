#pragma once

#include <chrono>
#include <cstdint>

inline uint64_t getCurrentTimeMs()
{
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}
