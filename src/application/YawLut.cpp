#include "sc/application/YawLut.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sc::application {
namespace {

struct CsvRow {
    float windSpeedBin;
    float windDirectionBin;
    YawLut::TurbineYawSetpoints yawValues;
};

struct BinBracket {
    int lowIndex;
    int highIndex;
    float weight;
};

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
                                        [](unsigned char ch) { return std::isspace(ch) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
                                       [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool isWindSpeedHeader(const std::string& token) {
    const std::string value = toLower(token);
    return value == "ws" || value == "ws_bin";
}

bool isWindDirectionHeader(const std::string& token) {
    const std::string value = toLower(token);
    return value == "wd" || value == "wd_bin";
}

bool isYawSetpointHeader(const std::string& token) {
    const std::string value = toLower(token);
    return !value.empty() && (value.rfind("yaw", 0) == 0 || value.rfind("wt", 0) == 0);
}

float parseFloat(const std::string& token) {
    std::size_t parsedCharacters = 0;
    const float value = std::stof(token, &parsedCharacters);
    if (parsedCharacters != token.size()) {
        throw std::invalid_argument("Unexpected trailing characters");
    }
    return value;
}

BinBracket findBracket(const std::vector<float>& bins, float value) {
    const float clamped = std::clamp(value, bins.front(), bins.back());
    if (clamped <= bins.front()) {
        return {0, 0, 0.0f};
    }
    if (clamped >= bins.back()) {
        const int lastIndex = static_cast<int>(bins.size() - 1);
        return {lastIndex, lastIndex, 0.0f};
    }

    const auto highIt = std::lower_bound(bins.begin(), bins.end(), clamped);
    const int highIndex = static_cast<int>(std::distance(bins.begin(), highIt));
    if (*highIt == clamped) {
        return {highIndex, highIndex, 0.0f};
    }

    const int lowIndex = highIndex - 1;
    const float span = bins[static_cast<std::size_t>(highIndex)] - bins[static_cast<std::size_t>(lowIndex)];
    const float weight = (clamped - bins[static_cast<std::size_t>(lowIndex)]) / span;
    return {lowIndex, highIndex, weight};
}

} // namespace

YawLut::YawLut(const std::string& csvFilePath) {
    std::ifstream csvFile(csvFilePath);
    if (!csvFile.is_open()) {
        throw std::runtime_error("Failed to open yaw LUT CSV: " + csvFilePath);
    }

    std::vector<CsvRow> rows;
    std::string line;
    std::size_t lineNumber = 0;
    std::size_t turbineCount = 0;
    std::size_t expectedColumnCount = 0;
    bool firstNonEmptyLine = true;

    while (std::getline(csvFile, line)) {
        ++lineNumber;

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (trim(line).empty()) {
            continue;
        }

        std::stringstream lineStream(line);
        std::vector<std::string> tokens;
        std::string token;
        while (std::getline(lineStream, token, ',')) {
            tokens.push_back(trim(token));
        }

        if (tokens.size() < 3) {
            throw std::runtime_error("Invalid yaw LUT row at line " + std::to_string(lineNumber));
        }

        if (firstNonEmptyLine) {
            firstNonEmptyLine = false;
            if (isWindSpeedHeader(tokens[0]) || isWindDirectionHeader(tokens[1])) {
                if (!isWindSpeedHeader(tokens[0]) || !isWindDirectionHeader(tokens[1])) {
                    throw std::runtime_error("Yaw LUT header must start with ws/ws_bin,wd/wd_bin");
                }
                for (std::size_t i = 2; i < tokens.size(); ++i) {
                    if (!isYawSetpointHeader(tokens[i])) {
                        throw std::runtime_error("Yaw LUT header columns after wd/wd_bin must be yaw or WT setpoints");
                    }
                }
                expectedColumnCount = tokens.size();
                continue;
            }
        }

        if (expectedColumnCount == 0) {
            expectedColumnCount = tokens.size();
        } else if (tokens.size() != expectedColumnCount) {
            throw std::runtime_error("Inconsistent yaw LUT column count at line " + std::to_string(lineNumber));
        }

        try {
            CsvRow row{};
            row.windSpeedBin = parseFloat(tokens[0]);
            row.windDirectionBin = parseFloat(tokens[1]);

            row.yawValues.reserve(tokens.size() - 2);
            for (std::size_t i = 2; i < tokens.size(); ++i) {
                row.yawValues.push_back(parseFloat(tokens[i]));
            }

            if (turbineCount == 0) {
                turbineCount = row.yawValues.size();
            } else if (row.yawValues.size() != turbineCount) {
                throw std::runtime_error(
                    "Inconsistent yaw setpoint column count at line " + std::to_string(lineNumber));
            }

            windSpeedBins_.push_back(row.windSpeedBin);
            windDirectionBins_.push_back(row.windDirectionBin);
            rows.push_back(std::move(row));
        } catch (const std::exception&) {
            throw std::runtime_error("Failed to parse yaw LUT row at line " + std::to_string(lineNumber));
        }
    }

    if (rows.empty()) {
        throw std::runtime_error("Yaw LUT CSV contains no data rows: " + csvFilePath);
    }

    std::sort(windSpeedBins_.begin(), windSpeedBins_.end());
    windSpeedBins_.erase(std::unique(windSpeedBins_.begin(), windSpeedBins_.end()), windSpeedBins_.end());
    std::sort(windDirectionBins_.begin(), windDirectionBins_.end());
    windDirectionBins_.erase(
        std::unique(windDirectionBins_.begin(), windDirectionBins_.end()), windDirectionBins_.end());

    if (windSpeedBins_.size() < 2) {
        throw std::runtime_error("Wind-speed must contain at least two unique bins");
    }
    if (windDirectionBins_.size() < 2) {
        throw std::runtime_error("Wind-direction must contain at least two unique bins");
    }

    yawSetpoints_.assign(
        windSpeedBins_.size(),
        std::vector<TurbineYawSetpoints>(windDirectionBins_.size(), TurbineYawSetpoints(turbineCount, 0.0f)));
    std::vector<std::vector<bool>> populated(
        windSpeedBins_.size(), std::vector<bool>(windDirectionBins_.size(), false));

    for (const auto& row : rows) {
        const auto windSpeedIt = std::lower_bound(windSpeedBins_.begin(), windSpeedBins_.end(), row.windSpeedBin);
        const auto windDirectionIt =
            std::lower_bound(windDirectionBins_.begin(), windDirectionBins_.end(), row.windDirectionBin);
        const std::size_t windSpeedIndex =
            static_cast<std::size_t>(std::distance(windSpeedBins_.begin(), windSpeedIt));
        const std::size_t windDirectionIndex =
            static_cast<std::size_t>(std::distance(windDirectionBins_.begin(), windDirectionIt));

        if (populated[windSpeedIndex][windDirectionIndex]) {
            throw std::runtime_error("Yaw LUT contains duplicate ws_bin/wd_bin combinations");
        }

        yawSetpoints_[windSpeedIndex][windDirectionIndex] = row.yawValues;
        populated[windSpeedIndex][windDirectionIndex] = true;
    }

    for (std::size_t windSpeedIndex = 0; windSpeedIndex < populated.size(); ++windSpeedIndex) {
        for (std::size_t windDirectionIndex = 0;
             windDirectionIndex < populated[windSpeedIndex].size();
             ++windDirectionIndex) {
            if (!populated[windSpeedIndex][windDirectionIndex]) {
                throw std::runtime_error(
                    "Yaw LUT is missing one or more wind-speed/wind-direction combinations");
            }
        }
    }
}

YawLut::TurbineYawSetpoints YawLut::lookup(float windSpeed, float windDirection) const {
    const BinBracket windSpeedBracket = findBracket(windSpeedBins_, windSpeed);
    const BinBracket windDirectionBracket = findBracket(windDirectionBins_, windDirection);

    const auto& setpointsLowLow =
        yawSetpoints_[static_cast<std::size_t>(windSpeedBracket.lowIndex)]
                     [static_cast<std::size_t>(windDirectionBracket.lowIndex)];
    const auto& setpointsLowHigh =
        yawSetpoints_[static_cast<std::size_t>(windSpeedBracket.lowIndex)]
                     [static_cast<std::size_t>(windDirectionBracket.highIndex)];
    const auto& setpointsHighLow =
        yawSetpoints_[static_cast<std::size_t>(windSpeedBracket.highIndex)]
                     [static_cast<std::size_t>(windDirectionBracket.lowIndex)];
    const auto& setpointsHighHigh =
        yawSetpoints_[static_cast<std::size_t>(windSpeedBracket.highIndex)]
                     [static_cast<std::size_t>(windDirectionBracket.highIndex)];

    TurbineYawSetpoints result(setpointsLowLow.size(), 0.0f);
    for (std::size_t i = 0; i < setpointsLowLow.size(); ++i) {
        result[i] = setpointsLowLow[i] * (1.0f - windSpeedBracket.weight) *
                        (1.0f - windDirectionBracket.weight) +
                    setpointsLowHigh[i] * (1.0f - windSpeedBracket.weight) * windDirectionBracket.weight +
                    setpointsHighLow[i] * windSpeedBracket.weight * (1.0f - windDirectionBracket.weight) +
                    setpointsHighHigh[i] * windSpeedBracket.weight * windDirectionBracket.weight;
    }
    return result;
}

} // namespace sc::application
