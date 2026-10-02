#include "sc/runtime/DataHistorian.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <system_error>

namespace {

class TemporaryDirectory {
    public:
    TemporaryDirectory()
    {
        static std::atomic<unsigned long> nextId{0};
        path_ = std::filesystem::temp_directory_path() /
                ("sc-historian-test-" + std::to_string(std::random_device{}()) + "-" + std::to_string(nextId.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        DataHistorian::instance().stopRun();
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const
    {
        return path_;
    }

    private:
    std::filesystem::path path_;
};

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("data historian restarts into a new file without losing the previous run")
{
    TemporaryDirectory directory;
    auto& historian = DataHistorian::instance();
    historian.stopRun();
    historian.configure("restart test", directory.path(), 1);

    historian.start();
    const auto firstPath = historian.currentFilePath();
    historian.start();
    REQUIRE(historian.currentFilePath() == firstPath);
    historian.log("run", 1.0);
    historian.stopRun();

    historian.start();
    const auto secondPath = historian.currentFilePath();
    historian.log("run", 2.0);
    historian.stopRun();

    REQUIRE(secondPath != firstPath);
    REQUIRE(readFile(firstPath).find(",run,1") != std::string::npos);
    REQUIRE(readFile(firstPath).find(",run,2") == std::string::npos);
    REQUIRE(readFile(secondPath).find(",run,2") != std::string::npos);
}

TEST_CASE("data historian preserves its mixed structured and raw log format")
{
    TemporaryDirectory directory;
    auto& historian = DataHistorian::instance();
    historian.stopRun();
    historian.configure("mixed log", directory.path(), 1);
    historian.start();
    const auto path = historian.currentFilePath();

    historian.log("power", 12.5);
    historian.log("[WT1]1000;YawAng=90.0");
    historian.stopRun();

    const std::string contents = readFile(path);
    REQUIRE(path.extension() == ".log");
    REQUIRE(contents.find(",power,12.5\n") != std::string::npos);
    REQUIRE(contents.find("[WT1]1000;YawAng=90.0\n") != std::string::npos);
    REQUIRE(contents.find("timestamp_ms,key,value") == std::string::npos);
}
