#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "ControlTask.hpp"
#include "common/config.hpp"
#include "common/GlobalDataStructure.hpp"

namespace {
    struct CsvRow {
        float wsBin;
        float wdBin;
        std::vector<float> yawValues;
    };

    struct BinBracket {
        int lowIndex;
        int highIndex;
        float weight;
    };

    std::string trim(std::string value) {
        const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        });
        const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        }).base();
        if (first >= last) {
            return std::string{};
        }
        return std::string(first, last);
    }

    std::string toLower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
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
        size_t parsedCharacters = 0;
        const float value = std::stof(token, &parsedCharacters);
        if (parsedCharacters != token.size()) {
            throw std::invalid_argument("Unexpected trailing characters");
        }
        return value;
    }

    BinBracket findBracket(const std::vector<float>& bins, float value) {
        const float clamped = std::clamp(value, bins.front(), bins.back());
        if (clamped <= bins.front()) {
            return BinBracket{0, 0, 0.0f};
        }
        if (clamped >= bins.back()) {
            const int lastIndex = static_cast<int>(bins.size() - 1);
            return BinBracket{lastIndex, lastIndex, 0.0f};
        }

        const auto highIt = std::lower_bound(bins.begin(), bins.end(), clamped);
        const int highIndex = static_cast<int>(std::distance(bins.begin(), highIt));
        if (*highIt == clamped) {
            return BinBracket{highIndex, highIndex, 0.0f};
        }

        const int lowIndex = highIndex - 1;
        const float span = bins[static_cast<size_t>(highIndex)] - bins[static_cast<size_t>(lowIndex)];
        const float weight = (clamped - bins[static_cast<size_t>(lowIndex)]) / span;
        return BinBracket{lowIndex, highIndex, weight};
    }
}

ControlTask::YawLut::YawLut(const std::string& csvFilePath) {
    std::ifstream csvFile(csvFilePath);
    if (!csvFile.is_open()) {
        throw std::runtime_error("Failed to open yaw LUT CSV: " + csvFilePath);
    }

    std::vector<CsvRow> rows;
    std::string line;
    size_t lineNumber = 0;
    size_t turbineCount = 0;
    size_t expectedColumnCount = 0;
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
                for (size_t i = 2; i < tokens.size(); ++i) {
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
            row.wsBin = parseFloat(tokens[0]);
            row.wdBin = parseFloat(tokens[1]);

            row.yawValues.reserve(tokens.size() - 2);
            for (size_t i = 2; i < tokens.size(); ++i) {
                row.yawValues.push_back(parseFloat(tokens[i]));
            }

            if (turbineCount == 0) {
                turbineCount = row.yawValues.size();
            } else if (row.yawValues.size() != turbineCount) {
                throw std::runtime_error("Inconsistent yaw setpoint column count at line " + std::to_string(lineNumber));
            }

            windSpeedBins_.push_back(row.wsBin);
            windDirectionBins_.push_back(row.wdBin);
            rows.push_back(row);
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
    windDirectionBins_.erase(std::unique(windDirectionBins_.begin(), windDirectionBins_.end()), windDirectionBins_.end());

    if (windSpeedBins_.size() < 2) {
        throw std::runtime_error("Wind-speed must contain at least two unique bins");
    }
    if (windDirectionBins_.size() < 2) {
        throw std::runtime_error("Wind-direction must contain at least two unique bins");
    }

    const size_t wsBinCount = windSpeedBins_.size();
    const size_t wdBinCount = windDirectionBins_.size();
    yawSetpoints_.assign(wsBinCount, std::vector<TurbineYawSetpoints>(wdBinCount, TurbineYawSetpoints(turbineCount, 0.0f)));
    std::vector<std::vector<bool>> populated(wsBinCount, std::vector<bool>(wdBinCount, false));

    for (const auto& row : rows) {
        const auto wsIt = std::lower_bound(windSpeedBins_.begin(), windSpeedBins_.end(), row.wsBin);
        const auto wdIt = std::lower_bound(windDirectionBins_.begin(), windDirectionBins_.end(), row.wdBin);
        const size_t wsIndex = static_cast<size_t>(std::distance(windSpeedBins_.begin(), wsIt));
        const size_t wdIndex = static_cast<size_t>(std::distance(windDirectionBins_.begin(), wdIt));

        if (populated[wsIndex][wdIndex]) {
            throw std::runtime_error("Yaw LUT contains duplicate ws_bin/wd_bin combinations");
        }

        yawSetpoints_[wsIndex][wdIndex] = row.yawValues;
        populated[wsIndex][wdIndex] = true;
    }

    for (size_t wsIndex = 0; wsIndex < populated.size(); ++wsIndex) {
        for (size_t wdIndex = 0; wdIndex < populated[wsIndex].size(); ++wdIndex) {
            if (!populated[wsIndex][wdIndex]) {
                throw std::runtime_error("Yaw LUT is missing one or more wind-speed/wind-direction combinations");
            }
        }
    }
}

ControlTask::YawLut::TurbineYawSetpoints ControlTask::YawLut::lookup(float ws, float wd) const {
    const BinBracket wsBracket = findBracket(windSpeedBins_, ws);
    const BinBracket wdBracket = findBracket(windDirectionBins_, wd);

    const auto& spLL = yawSetpoints_[static_cast<size_t>(wsBracket.lowIndex)][static_cast<size_t>(wdBracket.lowIndex)];
    const auto& spLH = yawSetpoints_[static_cast<size_t>(wsBracket.lowIndex)][static_cast<size_t>(wdBracket.highIndex)];
    const auto& spHL = yawSetpoints_[static_cast<size_t>(wsBracket.highIndex)][static_cast<size_t>(wdBracket.lowIndex)];
    const auto& spHH = yawSetpoints_[static_cast<size_t>(wsBracket.highIndex)][static_cast<size_t>(wdBracket.highIndex)];

    TurbineYawSetpoints result(spLL.size(), 0.0f);
    for (size_t i = 0; i < spLL.size(); ++i) {
        result[i] = spLL[i] * (1.0f - wsBracket.weight) * (1.0f - wdBracket.weight) +
                    spLH[i] * (1.0f - wsBracket.weight) * wdBracket.weight +
                    spHL[i] * wsBracket.weight * (1.0f - wdBracket.weight) +
                    spHH[i] * wsBracket.weight * wdBracket.weight;
    }
    return result;
}

ControlTask::ControlTask(Config config)
    : PeriodicTask(config.period),
      numTurbines_(config.numTurbines),
      yawLut_(config.yawLutCsvPath) {
}

void ControlTask::execute() {
    float powerSetpoint = 0.0f;
    float farmWindSpeed = 0.0f;
    float farmWindDirection = 0.0f;
    bool yawSteeringEnabled = false;

    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        const auto& gds = GlobalDataStructure::instance().data();
        powerSetpoint = gds.requestedReferencePower;
        farmWindSpeed = gds.farmWindSpeed;
        farmWindDirection = gds.farmWindDirection;
        yawSteeringEnabled = gds.yawSteeringEnabled;
    }

    CONTROL_LOG_V2("Using Wind Speed: " << farmWindSpeed << " m/s, Wind Direction: " << farmWindDirection
                  << " deg, to compute setpoints for requested reference power: " << powerSetpoint << " W");
    std::vector<float> powerSetpoints = std::vector<float>(numTurbines_, -1.0);
    std::vector<int> yawSetpoints = std::vector<int>(numTurbines_, farmWindDirection);
    
    if (yawSteeringEnabled) {
        // LUT values are yaw offsets; IEC expects an absolute yaw setpoint.
        yawSetpoints = std::vector<int>();
        const auto yawSetpointsFromLut = yawLut_.lookup(farmWindSpeed, farmWindDirection);
        for (const float value : yawSetpointsFromLut) {
            yawSetpoints.push_back(static_cast<int>(std::lround(farmWindDirection - value)));
        }
    }
    powerSetpoints = std::vector<float>(numTurbines_, static_cast<float>(powerSetpoint / numTurbines_));

#if SC_LOG_LEVEL_CONTROL >= 2
    std::ostringstream powerLine;
    for (const auto& sp : powerSetpoints) {
        powerLine << sp << " ";
    }
    CONTROL_LOG_V1("Computed power setpoints: " << powerLine.str());

    std::ostringstream yawLine;
    for (const auto& sp : yawSetpoints) {
        yawLine << sp << " ";
    }
    CONTROL_LOG_V1("Computed yaw setpoints: " << yawLine.str());
#endif

    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        for (int i = 0; i < numTurbines_; ++i) {
            GlobalDataStructure::instance().data().turbinePowerSetpoints[i] = powerSetpoints[i];
            GlobalDataStructure::instance().data().turbineYawSetpoints[i] = yawSetpoints[i];
        }
    }
}


void ControlTask::onStop() {
    CONTROL_LOG_V1("Stopped");
}
