#pragma once

#include <chrono>
#include <ctime>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <variant>

inline uint64_t getCurrentTimeMs()
{
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

inline std::string formatTime(uint64_t totalMs) {
    time_t totalSec = static_cast<time_t>(totalMs / 1000);
    int    ms = static_cast<int>(totalMs % 1000);

    std::tm* local = std::localtime(&totalSec); // swap for gmtime() if you want UTC

    std::ostringstream oss;
    oss << std::put_time(local, "%H:%M:%S")
        << '.'
        << std::setfill('0') << std::setw(3) << ms;

    return oss.str(); // e.g. "14:35:22.471"
}

inline std::string getCurrentTimeFormatted() {
    return formatTime(getCurrentTimeMs());
}


