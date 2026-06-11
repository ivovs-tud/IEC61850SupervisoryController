#include "common/DataHistorian.hpp"

#include <stdexcept>
#include <utility>

DataHistorian& DataHistorian::instance() {
    static DataHistorian inst;
    return inst;
}

DataHistorian::~DataHistorian() {
    flush();
}

void DataHistorian::configure(std::string experimentName,
                              std::filesystem::path outputDir,
                              std::size_t flushEvery,
                              std::chrono::milliseconds flushPeriod) {
    std::lock_guard<std::mutex> lock(mutex_);
    outputDir_ = std::move(outputDir);
    flushEvery_ = (flushEvery == 0 ? 1 : flushEvery);
    flushPeriod_ = flushPeriod;
    currentExperimentName_ = sanitizeName(std::move(experimentName));
}

void DataHistorian::start() {
    startNewRun(currentExperimentName_);
}

void DataHistorian::startNewRun(std::string experimentName) {
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

void DataHistorian::stopRun() {
    std::lock_guard<std::mutex> lock(mutex_);
    flushUnlocked();
    started_ = false;
}

void DataHistorian::log(const std::string& key, double value) {
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

void DataHistorian::log(const std::string& rawMessage) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) {
        return;
    }

    ensureFileOpen();
    buffer_ << rawMessage << '\n';
    ++bufferedRecords_;
    maybeFlushUnlocked();
}

void DataHistorian::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    flushUnlocked();
}

const std::filesystem::path& DataHistorian::currentFilePath() const noexcept {
    return filePath_;
}

std::string DataHistorian::sanitizeName(std::string name) {
    if (name.empty()) {
        return "run";
    }

    for (char& ch : name) {
        const bool isSafe = (ch >= 'a' && ch <= 'z') ||
                            (ch >= 'A' && ch <= 'Z') ||
                            (ch >= '0' && ch <= '9') ||
                            ch == '-' || ch == '_';
        if (!isSafe) {
            ch = '_';
        }
    }

    return name;
}

void DataHistorian::ensureFileOpen() {
    if (!file_.is_open()) {
        throw std::runtime_error("DataHistorian has no active output file");
    }
}

void DataHistorian::flushUnlocked() {
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

void DataHistorian::maybeFlushUnlocked() {
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
