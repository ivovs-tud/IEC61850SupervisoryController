#include "sc/runtime/Console.hpp"

#include <mutex>

namespace sc::runtime {
namespace {

std::mutex consoleMutex;

} // namespace

void writeConsoleText(std::ostream& stream, std::string_view text)
{
    std::lock_guard<std::mutex> lock(consoleMutex);
    stream << text << '\n';
    stream.flush();
}

} // namespace sc::runtime
