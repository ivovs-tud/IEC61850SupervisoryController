#include "sc/runtime/Console.hpp"

#include <catch2/catch_test_macros.hpp>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

TEST_CASE("console output keeps concurrent lines intact")
{
    constexpr int threadCount = 8;
    constexpr int linesPerThread = 100;
    std::ostringstream output;
    std::vector<std::thread> threads;

    for (int thread = 0; thread < threadCount; ++thread) {
        threads.emplace_back([&output, thread, linesPerThread]() {
            for (int line = 0; line < linesPerThread; ++line) {
                sc::runtime::writeConsoleLine(output, "thread=", thread, ";line=", line);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    std::set<std::string> actualLines;
    std::istringstream lines(output.str());
    for (std::string line; std::getline(lines, line);) {
        actualLines.insert(line);
    }

    REQUIRE(actualLines.size() == threadCount * linesPerThread);
    for (int thread = 0; thread < threadCount; ++thread) {
        for (int line = 0; line < linesPerThread; ++line) {
            const std::string expected = "thread=" + std::to_string(thread) + ";line=" + std::to_string(line);
            REQUIRE(actualLines.count(expected) == 1);
        }
    }
}
