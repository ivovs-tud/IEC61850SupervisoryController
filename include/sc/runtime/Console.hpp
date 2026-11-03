#pragma once

#include <ostream>
#include <sstream>
#include <string_view>
#include <utility>

namespace sc::runtime {

/** Write one complete line without interleaving output from other threads. */
void writeConsoleText(std::ostream& stream, std::string_view text);

template <typename... Values> void writeConsoleLine(std::ostream& stream, Values&&... values)
{
    std::ostringstream line;
    (line << ... << std::forward<Values>(values));
    writeConsoleText(stream, line.str());
}

} // namespace sc::runtime
