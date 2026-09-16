#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

namespace sc::test {

class TemporaryCsv {
public:
    explicit TemporaryCsv(const std::string& contents) {
        static std::atomic<unsigned long> nextId{0};
        path_ = std::filesystem::temp_directory_path() /
                ("sc-test-" + std::to_string(std::random_device{}()) + "-" +
                 std::to_string(nextId.fetch_add(1)) + ".csv");

        std::ofstream file(path_, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to create temporary CSV");
        }
        file << contents;
        if (!file) {
            throw std::runtime_error("Failed to write temporary CSV");
        }
    }

    ~TemporaryCsv() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    TemporaryCsv(const TemporaryCsv&) = delete;
    TemporaryCsv& operator=(const TemporaryCsv&) = delete;

    std::string path() const {
        return path_.string();
    }

private:
    std::filesystem::path path_;
};

} // namespace sc::test
