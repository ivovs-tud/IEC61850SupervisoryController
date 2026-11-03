#pragma once

#include "sc/model/SharedData.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

inline constexpr int DEFAULT_HMI_SIGNAL_WINDOW_SIZE = 300;

struct HmiData {
    std::vector<double> turbinePower;
    std::vector<double> turbineYaw;
    std::vector<double> turbineWindSpeed;
    std::vector<double> turbineWindDirection;
    std::vector<double> turbineRotorSpeed;
    std::vector<double> turbineGeneratorTorque;
    std::vector<float> powerSetpoints;
    std::vector<float> yawSetpoints;
    double requestedPower{0.0};
    double measuredTotalPower{0.0};
    double totalReceivedPower{0.0};
    double farmWindSpeed{0.0};
    double farmWindDirection{0.0};
    int operationMode{0};
    int connectedTurbines{0};
    bool systemRunning{false};
    bool yawSteeringEnabled{false};
    std::string yawSteeringCommandName;
    std::vector<uint32_t> turbineEnabled;
    bool alarmWRecMeas{false};
    bool alarmOrientationMisalign{false};
    bool alarmWTorqueRotSpd{false};
    bool alarmPowerExpected{false};
    bool alarmHorWdDir{false};
    bool alarmHorWdDirChg{false};
    bool alarmHorWdSpdChg{false};
    bool alarmTelemetryFreezeReplay{false};
    bool alarmDrivetrainUnderResponse{false};
    bool alarmStaticBounds{false};
    bool alarmFleetPeerOutlier{false};
    int attackTapEnabled{0};
    int attackTapAvailable{0};
    int attackFdiEnabled{0};
    int attackFdiAvailable{0};
    std::vector<std::string> attackFdiSignals;
};

struct HmiSignalDef {
    std::string name;
    std::string unit;
    std::vector<std::string> lineLabels;
    std::function<std::vector<double>(const HmiData&)> accessor;
    std::optional<std::pair<double, double>> defaultYRange = std::nullopt;
};

struct HmiConfig {
    int numTurbines = static_cast<int>(DEFAULT_TURBINE_COUNT);
    int windowSize = DEFAULT_HMI_SIGNAL_WINDOW_SIZE;
#ifdef _WIN32
    std::string publisherEndpoint = "tcp://127.0.0.1:5555";
    std::string commandEndpoint = "tcp://127.0.0.1:5556";
#else
    std::string publisherEndpoint = "ipc:///tmp/supervisory_controller_hmi.sock";
    std::string commandEndpoint = "ipc:///tmp/supervisory_controller_hmi_cmd.sock";
#endif
    bool alarmAcknowledgementEnabled = false;
    std::vector<HmiSignalDef> signals;
};

HmiData collectHmiData(const SharedData& data);
HmiConfig defaultHmiConfig(int numTurbines = static_cast<int>(DEFAULT_TURBINE_COUNT));
