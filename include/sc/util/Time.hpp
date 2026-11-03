#pragma once

#include <cstdint>

namespace sc::util {

bool isTimestampRecent(uint64_t timestampMs, uint64_t currentTimeMs, uint64_t timeoutMs);

} // namespace sc::util
