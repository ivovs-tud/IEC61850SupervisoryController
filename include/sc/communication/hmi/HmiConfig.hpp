#pragma once

#include "sc/model/SharedData.hpp"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

inline constexpr int DEFAULT_HMI_SIGNAL_WINDOW_SIZE = 300;

struct HmiSignalDef {
    std::string name;
    std::string unit;
    std::vector<std::string> lineLabels;
    std::function<std::vector<double>(const SharedData&)> accessor;
    std::optional<std::pair<double, double>> defaultYRange = std::nullopt;
};

struct HmiConfig {
    int numTurbines = static_cast<int>(DEFAULT_TURBINE_COUNT);
    int windowSize = DEFAULT_HMI_SIGNAL_WINDOW_SIZE;
    std::string publisherEndpoint = "ipc:///tmp/supervisory_controller_hmi.sock";
    std::string commandEndpoint = "ipc:///tmp/supervisory_controller_hmi_cmd.sock";
    bool alarmAcknowledgementEnabled = false;
    std::vector<HmiSignalDef> signals;
};

HmiConfig defaultHmiConfig(int numTurbines = static_cast<int>(DEFAULT_TURBINE_COUNT));
