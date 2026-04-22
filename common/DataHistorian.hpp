#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

// ---------------------------------------------------------------------------
// DataHistorian – lightweight CSV logger.
//
// Design goals:
// - one file per experiment/run
// - caller provides a human-readable run name
// - current UNIX timestamp in seconds is appended to the file name
// - thread-safe logging from different tasks
// - buffered writes to keep runtime overhead low
//
// File format: CSV with columns `timestamp_ms,key,value`
// Example filename: data/baseline_run_1775551234.csv
// ---------------------------------------------------------------------------
class DataHistorian
{
public:
    static DataHistorian& instance()
    {
        static DataHistorian inst;
        return inst;
    }

    DataHistorian(const DataHistorian&) = delete;
    DataHistorian& operator=(const DataHistorian&) = delete;
    DataHistorian(DataHistorian&&) = delete;
    DataHistorian& operator=(DataHistorian&&) = delete;

    ~DataHistorian()
    {
        flush();
    }

    void configure(std::string experimentName = "run",
                   std::filesystem::path outputDir = "data",
                   std::size_t flushEvery = 512,
                   std::chrono::milliseconds flushPeriod = std::chrono::milliseconds(5000))
    {
        std::lock_guard<std::mutex> lock(mutex_);
        outputDir_ = std::move(outputDir);
        flushEvery_ = (flushEvery == 0 ? 1 : flushEvery);
        flushPeriod_ = flushPeriod;
        currentExperimentName_ = sanitizeName(std::move(experimentName));
    }

    void start() {
        startNewRun(currentExperimentName_); 
    }

    void startNewRun(std::string experimentName)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        flushUnlocked();
        if (file_.is_open()) {
            file_.close();
        }

        std::filesystem::create_directories(outputDir_);

        currentExperimentName_ = sanitizeName(std::move(experimentName));
        const auto timestampSeconds = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        filePath_ = outputDir_ / (currentExperimentName_ + "_" + std::to_string(timestampSeconds) + ".log");
        file_.open(filePath_, std::ios::out | std::ios::trunc);

        if (!file_.is_open()) {
            throw std::runtime_error("DataHistorian failed to open file: " + filePath_.string());
        }

        file_ << "timestamp_ms,key,value\n";
        file_.flush();
        lastFlushTime_ = std::chrono::steady_clock::now();
        started_ = true;
    }

    void stopRun()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        flushUnlocked();
        started_ = false;
    }

    // Log a named scalar measurement.
    void log(const std::string& key, double value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return;
        }

        const auto timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        ensureFileOpen();

        buffer_ << timestampMs << ',' << key << ',' << value << '\n';
        ++bufferedRecords_;
        maybeFlushUnlocked();
    }

    // Log a raw text message while prepending a timestamp.
    void log(const std::string& rawMessage)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return;
        }

        // const auto timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        //     std::chrono::system_clock::now().time_since_epoch()).count();
        ensureFileOpen();

        // buffer_ << timestampMs << ' ' << rawMessage << '\n';
        buffer_ << rawMessage << '\n';
        ++bufferedRecords_;
        maybeFlushUnlocked();
    }

    // Flush buffered records to the backing store.
    void flush()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        flushUnlocked();
    }

    const std::filesystem::path& currentFilePath() const noexcept
    {
        return filePath_;
    }

private:
    DataHistorian() = default;

    static std::string sanitizeName(std::string name)
    {
        if (name.empty()) {
            return "run";
        }

        for (char& ch : name) {
            const bool isSafe =
                (ch >= 'a' && ch <= 'z') ||
                (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') ||
                ch == '-' || ch == '_';

            if (!isSafe) {
                ch = '_';
            }
        }

        return name;
    }

    void ensureFileOpen()
    {
        if (!file_.is_open()) {
            throw std::runtime_error("DataHistorian has no active output file");
        }
    }

    void flushUnlocked()
    {
        if (!file_.is_open() || bufferedRecords_ == 0) {
            return;
        }

        file_ << buffer_.str();
        file_.flush();

        buffer_.str("");
        buffer_.clear();
        bufferedRecords_ = 0;
        lastFlushTime_ = std::chrono::steady_clock::now();
    }

    void maybeFlushUnlocked()
    {
        if (bufferedRecords_ >= flushEvery_) {
            flushUnlocked();
            return;
        }

        if (flushPeriod_.count() <= 0) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if ((now - lastFlushTime_) >= flushPeriod_) {
            flushUnlocked();
        }
    }

    std::filesystem::path outputDir_;
    std::filesystem::path filePath_;
    std::ofstream file_;
    std::mutex mutex_;
    std::ostringstream buffer_;
    std::string currentExperimentName_ {"run"};
    std::size_t flushEvery_ {64};
    std::chrono::milliseconds flushPeriod_{std::chrono::milliseconds(1000)};
    std::chrono::steady_clock::time_point lastFlushTime_{std::chrono::steady_clock::now()};
    std::size_t bufferedRecords_ {0};
    bool started_ {false};
};
