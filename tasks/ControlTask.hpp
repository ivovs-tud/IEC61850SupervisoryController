#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/PeriodicTask.hpp"

// ---------------------------------------------------------------------------
// ControlTask – closed-loop control algorithm.
// ---------------------------------------------------------------------------
class ControlTask : public PeriodicTask
{
public:
    struct Config {
        int numTurbines;
        std::chrono::milliseconds period{std::chrono::milliseconds(10)};
        std::string yawLutCsvPath{"yaw_lut.csv"};
    };

    explicit ControlTask(Config config);

protected:
    void execute() override;
    void onStop()  override;  // stops socket servers after the loop exits

private:
    class YawLUT {
        /**
         * @brief Class implementing a lookup table for yaw setpoints based on wind speed and direction bins.
         * Performs linear interpolation between bins for smooth setpoint transitions.
         *
         * TODO: Promote to standalone class if need for multiple LUTs arises.
         */

        using TurbYawSetpoints = std::vector<float>;

    public:
        explicit YawLUT(const std::string& csvFilePath)
        {
            /**
             * @brief Loads the LUT from a CSV file. The CSV is expected to have a specific format:
             * ws_bin,wd_bin,yaw1,yaw2,...,yawN
             */

            struct CsvRow {
                float ws_bin;
                float wd_bin;
                TurbYawSetpoints yaw_values;
            };

            const auto trim = [](std::string value) {
                const auto first = std::find_if_not(value.begin(), value.end(),
                                                    [](unsigned char ch) { return std::isspace(ch) != 0; });
                const auto last = std::find_if_not(value.rbegin(), value.rend(),
                                                   [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
                if (first >= last) {
                    return std::string{};
                }
                return std::string(first, last);
            };

            const auto parseFloat = [](const std::string& token) {
                size_t parsedCharacters = 0;
                const float value = std::stof(token, &parsedCharacters);
                if (parsedCharacters != token.size()) {
                    throw std::invalid_argument("Unexpected trailing characters");
                }
                return value;
            };

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
                    if (tokens[0] == "ws_bin" || tokens[1] == "wd_bin") {
                        if (tokens[0] != "ws_bin" || tokens[1] != "wd_bin") {
                            throw std::runtime_error("Yaw LUT header must start with ws_bin,wd_bin");
                        }
                        for (size_t i = 2; i < tokens.size(); ++i) {
                            if (tokens[i].empty() || tokens[i].rfind("yaw", 0) != 0) {
                                throw std::runtime_error("Yaw LUT header columns after wd_bin must be yaw setpoints");
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
                    row.ws_bin = parseFloat(tokens[0]);
                    row.wd_bin = parseFloat(tokens[1]);

                    row.yaw_values.reserve(tokens.size() - 2);
                    for (size_t i = 2; i < tokens.size(); ++i) {
                        row.yaw_values.push_back(parseFloat(tokens[i]));
                    }

                    if (turbineCount == 0) {
                        turbineCount = row.yaw_values.size();
                    } else if (row.yaw_values.size() != turbineCount) {
                        throw std::runtime_error(
                            "Inconsistent yaw setpoint column count at line " + std::to_string(lineNumber));
                    }

                    ws_bins_.push_back(row.ws_bin);
                    wd_bins_.push_back(row.wd_bin);
                    rows.push_back(row);
                } catch (const std::exception&) {
                    throw std::runtime_error("Failed to parse yaw LUT row at line " + std::to_string(lineNumber));
                }
            }

            if (rows.empty()) {
                throw std::runtime_error("Yaw LUT CSV contains no data rows: " + csvFilePath);
            }

            std::sort(ws_bins_.begin(), ws_bins_.end());
            ws_bins_.erase(std::unique(ws_bins_.begin(), ws_bins_.end()), ws_bins_.end());
            std::sort(wd_bins_.begin(), wd_bins_.end());
            wd_bins_.erase(std::unique(wd_bins_.begin(), wd_bins_.end()), wd_bins_.end());

            ws_bin_count_ = static_cast<int>(ws_bins_.size());
            wd_bin_count_ = static_cast<int>(wd_bins_.size());

            ws_min_ = ws_bins_.front();
            ws_max_ = ws_bins_.back();
            wd_min_ = wd_bins_.front();
            wd_max_ = wd_bins_.back();

            const auto computeBinSize = [](const std::vector<float>& bins, const std::string& axisName) {
                if (bins.size() < 2) {
                    throw std::runtime_error(axisName + " must contain at least two unique bins");
                }

                const float binSize = bins[1] - bins[0];
                if (binSize <= 0.0f) {
                    throw std::runtime_error(axisName + " bins must be strictly increasing");
                }

                constexpr float tolerance = 1e-4f;
                for (size_t i = 2; i < bins.size(); ++i) {
                    const float diff = bins[i] - bins[i - 1];
                    if (std::abs(diff - binSize) > tolerance) {
                        throw std::runtime_error(axisName + " bins must be uniformly spaced for direct index lookup");
                    }
                }

                return binSize;
            };

            ws_bin_size_ = computeBinSize(ws_bins_, "Wind-speed");
            wd_bin_size_ = computeBinSize(wd_bins_, "Wind-direction");

            yaw_setpoints_.assign(static_cast<size_t>(ws_bin_count_),
                                  std::vector<TurbYawSetpoints>(static_cast<size_t>(wd_bin_count_),
                                                                TurbYawSetpoints(turbineCount, 0.0f)));
            std::vector<std::vector<bool>> populated(static_cast<size_t>(ws_bin_count_),
                                                     std::vector<bool>(static_cast<size_t>(wd_bin_count_), false));

            for (const auto& row : rows) {
                const auto wsIt = std::lower_bound(ws_bins_.begin(), ws_bins_.end(), row.ws_bin);
                const auto wdIt = std::lower_bound(wd_bins_.begin(), wd_bins_.end(), row.wd_bin);
                const size_t wsIndex = static_cast<size_t>(std::distance(ws_bins_.begin(), wsIt));
                const size_t wdIndex = static_cast<size_t>(std::distance(wd_bins_.begin(), wdIt));

                if (populated[wsIndex][wdIndex]) {
                    throw std::runtime_error("Yaw LUT contains duplicate ws_bin/wd_bin combinations");
                }

                yaw_setpoints_[wsIndex][wdIndex] = row.yaw_values;
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

        TurbYawSetpoints lookup(float ws, float wd) const
        {
            /**
             * @brief Looks up the yaw setpoints for the given wind speed and direction using linear interpolation between the appropriate bins.
             */

            const float ws_clamped = std::clamp(ws, ws_min_, ws_max_);
            const int ws_idx_low = std::max(0, std::min(ws_bin_count_ - 1,
                                                        static_cast<int>((ws_clamped - ws_min_) / ws_bin_size_)));
            const int ws_idx_high = std::max(0, std::min(ws_bin_count_ - 1, ws_idx_low + 1));
            const float ws_weight = std::clamp((ws_clamped - ws_bins_[static_cast<size_t>(ws_idx_low)]) / ws_bin_size_,
                                               0.0f, 1.0f);

            const float wd_clamped = std::clamp(wd, wd_min_, wd_max_);
            const int wd_idx_low = std::max(0, std::min(wd_bin_count_ - 1,
                                                        static_cast<int>((wd_clamped - wd_min_) / wd_bin_size_)));
            const int wd_idx_high = std::max(0, std::min(wd_bin_count_ - 1, wd_idx_low + 1));
            const float wd_weight = std::clamp((wd_clamped - wd_bins_[static_cast<size_t>(wd_idx_low)]) / wd_bin_size_,
                                               0.0f, 1.0f);

            const auto& sp_ll = yaw_setpoints_[static_cast<size_t>(ws_idx_low)][static_cast<size_t>(wd_idx_low)];
            const auto& sp_lh = yaw_setpoints_[static_cast<size_t>(ws_idx_low)][static_cast<size_t>(wd_idx_high)];
            const auto& sp_hl = yaw_setpoints_[static_cast<size_t>(ws_idx_high)][static_cast<size_t>(wd_idx_low)];
            const auto& sp_hh = yaw_setpoints_[static_cast<size_t>(ws_idx_high)][static_cast<size_t>(wd_idx_high)];

            TurbYawSetpoints result(sp_ll.size(), 0.0f);
            for (size_t i = 0; i < sp_ll.size(); ++i) {
                result[i] = sp_ll[i] * (1.0f - ws_weight) * (1.0f - wd_weight) +
                            sp_lh[i] * (1.0f - ws_weight) * wd_weight +
                            sp_hl[i] * ws_weight * (1.0f - wd_weight) +
                            sp_hh[i] * ws_weight * wd_weight;
            }
            return result;
        }

    private:
        std::vector<float> ws_bins_;
        float ws_bin_size_{0.0f};
        int ws_bin_count_{0};
        float ws_min_{0.0f};
        float ws_max_{0.0f};

        std::vector<float> wd_bins_;
        float wd_bin_size_{0.0f};
        int wd_bin_count_{0};
        float wd_min_{0.0f};
        float wd_max_{0.0f};

        std::vector<std::vector<TurbYawSetpoints>> yaw_setpoints_;
    };

    YawLUT yawLut_;
    int numTurbines_;
};
