#include "sc/runtime/DataHistorian.hpp"

#include <stdexcept>
#include <utility>

DataHistorian& DataHistorian::instance()
{
    static DataHistorian historian;
    return historian;
}

DataHistorian::~DataHistorian()
{
    stopRun();
}

void DataHistorian::configure(std::string experimentName, std::filesystem::path outputDir, std::size_t flushEvery,
                              std::chrono::milliseconds flushPeriod)
{
    std::lock_guard<std::mutex> lock(mutex_);
    outputDir_ = std::move(outputDir);
    flushEvery_ = flushEvery == 0 ? 1 : flushEvery;
    flushPeriod_ = flushPeriod;
    currentExperimentName_ = sanitizeName(std::move(experimentName));
}

void DataHistorian::start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) {
        startNewRunUnlocked(currentExperimentName_);
    }
}

void DataHistorian::startNewRun(std::string experimentName)
{
    std::lock_guard<std::mutex> lock(mutex_);
    startNewRunUnlocked(std::move(experimentName));
}

void DataHistorian::startNewRunUnlocked(std::string experimentName)
{
    flushUnlocked();
    if (file_.is_open()) {
        file_.close();
    }
    started_ = false;

    std::filesystem::create_directories(outputDir_);
    currentExperimentName_ = sanitizeName(std::move(experimentName));
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    const std::string baseName = currentExperimentName_ + "_" + std::to_string(timestampMs);
    filePath_ = outputDir_ / (baseName + ".log");
    std::size_t suffix = 1;
    while (std::filesystem::exists(filePath_)) {
        filePath_ = outputDir_ / (baseName + "_" + std::to_string(suffix++) + ".log");
    }

    // Append mode prevents an unexpected filesystem race from truncating an existing run.
    // Binary mode keeps the log format on LF line endings on Windows.
    file_.open(filePath_, std::ios::out | std::ios::app | std::ios::binary);
    if (!file_.is_open()) {
        throw std::runtime_error("DataHistorian failed to open file: " + filePath_.string());
    }

    lastFlushTime_ = std::chrono::steady_clock::now();
    started_ = true;
}

void DataHistorian::stopRun()
{
    std::lock_guard<std::mutex> lock(mutex_);
    flushUnlocked();
    if (file_.is_open()) {
        file_.close();
    }
    started_ = false;
}

void DataHistorian::log(const std::string& key, double value)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) {
        return;
    }

    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    ensureFileOpen();
    buffer_ << timestampMs << ',' << key << ',' << value << '\n';
    ++bufferedRecords_;
    maybeFlushUnlocked();
}

void DataHistorian::log(const std::string& rawMessage)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) {
        return;
    }

    ensureFileOpen();
    buffer_ << rawMessage << '\n';
    ++bufferedRecords_;
    maybeFlushUnlocked();
}

void DataHistorian::flush()
{
    std::lock_guard<std::mutex> lock(mutex_);
    flushUnlocked();
}

const std::filesystem::path& DataHistorian::currentFilePath() const noexcept
{
    return filePath_;
}

std::string DataHistorian::sanitizeName(std::string name)
{
    if (name.empty()) {
        return "run";
    }

    for (char& character : name) {
        const bool safe = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') || character == '-' || character == '_';
        if (!safe) {
            character = '_';
        }
    }

    return name;
}

void DataHistorian::ensureFileOpen()
{
    if (!file_.is_open()) {
        throw std::runtime_error("DataHistorian has no active output file");
    }
}

void DataHistorian::flushUnlocked()
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

void DataHistorian::maybeFlushUnlocked()
{
    if (bufferedRecords_ >= flushEvery_) {
        flushUnlocked();
        return;
    }

    if (flushPeriod_.count() <= 0) {
        return;
    }

    if (std::chrono::steady_clock::now() - lastFlushTime_ >= flushPeriod_) {
        flushUnlocked();
    }
}
