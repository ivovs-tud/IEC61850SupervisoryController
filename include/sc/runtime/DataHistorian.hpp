#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

class DataHistorian {
public:
    static DataHistorian& instance();

    DataHistorian(const DataHistorian&) = delete;
    DataHistorian& operator=(const DataHistorian&) = delete;
    DataHistorian(DataHistorian&&) = delete;
    DataHistorian& operator=(DataHistorian&&) = delete;

    ~DataHistorian();

    void configure(std::string experimentName = "run", std::filesystem::path outputDir = "data", std::size_t flushEvery = 512, 
                    std::chrono::milliseconds flushPeriod = std::chrono::milliseconds(5000));
    void start();
    void startNewRun(std::string experimentName);
    void stopRun();
    void log(const std::string& key, double value);
    void log(const std::string& rawMessage);
    void flush();
    const std::filesystem::path& currentFilePath() const noexcept;

private:
    DataHistorian() = default;

    static std::string sanitizeName(std::string name);
    void ensureFileOpen();
    void flushUnlocked();
    void maybeFlushUnlocked();

    std::filesystem::path outputDir_;
    std::filesystem::path filePath_;
    std::ofstream file_;
    std::mutex mutex_;
    std::ostringstream buffer_;
    std::string currentExperimentName_{"run"};
    std::size_t flushEvery_{64};
    std::chrono::milliseconds flushPeriod_{std::chrono::milliseconds(1000)};
    std::chrono::steady_clock::time_point lastFlushTime_{std::chrono::steady_clock::now()};
    std::size_t bufferedRecords_{0};
    bool started_{false};
};
