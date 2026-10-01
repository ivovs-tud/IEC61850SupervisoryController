#include "sc/util/Time.hpp"

namespace sc::util {

bool isTimestampRecent(uint64_t timestampMs, uint64_t currentTimeMs, uint64_t timeoutMs) {
    return timestampMs != 0 && timestampMs <= currentTimeMs && currentTimeMs - timestampMs <= timeoutMs;
}

} // namespace sc::util
