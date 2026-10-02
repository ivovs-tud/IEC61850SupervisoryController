#pragma once

#include <boost/circular_buffer.hpp>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

inline constexpr std::size_t DEFAULT_TURBINE_COUNT = 9;

template <typename T>
using History = boost::circular_buffer<T>;

template <typename T>
using TurbineHistory = std::vector<History<T>>;

template <typename T>
inline TurbineHistory<T> makeTurbineHistory(std::size_t turbineCount, std::size_t capacity) {
    return TurbineHistory<T>(turbineCount, History<T>(capacity));
}

// Raw values written by turbine communication
struct CollectedData {
    // Wall-clock coverage follows the acquisition cadence.
    static constexpr int historySampleCapacity = 20;

    mutable std::mutex mutex;
    std::vector<double> lastWS = std::vector<double>(DEFAULT_TURBINE_COUNT, 0.0);
    std::vector<uint64_t> lastWS_t = std::vector<uint64_t>(DEFAULT_TURBINE_COUNT, 0);
    std::vector<double> lastWD = std::vector<double>(DEFAULT_TURBINE_COUNT, 0.0);
    std::vector<uint64_t> lastWD_t = std::vector<uint64_t>(DEFAULT_TURBINE_COUNT, 0);
    std::vector<double> lastYawOffset = std::vector<double>(DEFAULT_TURBINE_COUNT, 0.0);
    std::vector<uint64_t> lastYawOffset_t = std::vector<uint64_t>(DEFAULT_TURBINE_COUNT, 0);
    std::vector<double> lastRPM = std::vector<double>(DEFAULT_TURBINE_COUNT, 0.0);
    std::vector<uint64_t> lastRPM_t = std::vector<uint64_t>(DEFAULT_TURBINE_COUNT, 0);
    std::vector<double> lastPower = std::vector<double>(DEFAULT_TURBINE_COUNT, 0.0);
    std::vector<uint64_t> lastPower_t = std::vector<uint64_t>(DEFAULT_TURBINE_COUNT, 0);
    std::vector<double> lastGenTorque = std::vector<double>(DEFAULT_TURBINE_COUNT, 0.0);
    std::vector<uint64_t> lastGenTorque_t = std::vector<uint64_t>(DEFAULT_TURBINE_COUNT, 0);

    TurbineHistory<double> wsHistory = makeTurbineHistory<double>(DEFAULT_TURBINE_COUNT, historySampleCapacity);
    TurbineHistory<double> wdHistory = makeTurbineHistory<double>(DEFAULT_TURBINE_COUNT, historySampleCapacity);
    TurbineHistory<double> yawOffsetHistory = makeTurbineHistory<double>(DEFAULT_TURBINE_COUNT, historySampleCapacity);
    TurbineHistory<double> rpmHistory = makeTurbineHistory<double>(DEFAULT_TURBINE_COUNT, historySampleCapacity);
    TurbineHistory<double> powerHistory = makeTurbineHistory<double>(DEFAULT_TURBINE_COUNT, historySampleCapacity);
    TurbineHistory<double> genTorqueHistory = makeTurbineHistory<double>(DEFAULT_TURBINE_COUNT, historySampleCapacity);

    // Power before attack-interface overrides
    std::vector<double> measuredPower = std::vector<double>(DEFAULT_TURBINE_COUNT, 0.0);
};

// Farm-level values produced by SignalProcessingTask
struct ProcessedData {
    mutable std::mutex mutex;
    int connectedTurbines{0};
    std::vector<double> availablePower = std::vector<double>(DEFAULT_TURBINE_COUNT, 0.0);
    std::vector<double> filteredWindSpeeds = std::vector<double>(DEFAULT_TURBINE_COUNT, 0.0);
    std::vector<uint64_t> filteredWindSpeedTimeMs = std::vector<uint64_t>(DEFAULT_TURBINE_COUNT, 0);
    double totalReceivedPower{0.0};
    float windSpeed{0.0f};
    float windDirection{270.0f};
    History<double> measuredTotalPowerHistory = History<double>(CollectedData::historySampleCapacity);
};

// Operator inputs and commands sent to turbines
struct ControlData {
    static constexpr uint32_t controllerKomega2 = 1;
    static constexpr uint32_t controllerDownregulation = 2;
    static constexpr uint32_t controllerShutdown = 3;

    mutable std::mutex mutex;
    float requestedPower{-1.0f};
    bool yawSteeringEnabled{false};
    std::string yawSteeringCommandName{"Yaw Steering"};
    std::vector<float> powerSetpoints = std::vector<float>(DEFAULT_TURBINE_COUNT, -1.0f);
    std::vector<float> yawSetpoints = std::vector<float>(DEFAULT_TURBINE_COUNT, 0.0f);
    std::vector<uint32_t> turbineEnabled = std::vector<uint32_t>(DEFAULT_TURBINE_COUNT, 1);
    std::vector<uint32_t> turbineController = std::vector<uint32_t>(DEFAULT_TURBINE_COUNT, controllerKomega2);
    bool alarmAcknowledgementRequested{false};
    std::string statusMessage;
};

// Alarm flags written by MonitoringTask
struct MonitoringData {
    mutable std::mutex mutex;
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
};

// Simulator, connection and attack-interface status
struct InterfaceData {
    mutable std::mutex mutex;
    bool systemRunning{false};
    bool simStarted{false};
    bool simConfigured{false};
    int attackTapEnabled{0};
    int attackTapAvailable{0};
    int attackFdiEnabled{0};
    int attackFdiAvailable{0};
    std::vector<std::string> attackFdiSignals;
};

struct SharedData {
    static SharedData& instance() {
        static SharedData data;
        return data;
    }

    CollectedData collected;
    ProcessedData processed;
    ControlData control;
    MonitoringData monitoring;
    InterfaceData interface;

    void configureTurbineCount(std::size_t turbineCount) {
        if (turbineCount == 0) {
            throw std::invalid_argument("SharedData requires at least one turbine");
        }

        std::scoped_lock lock(collected.mutex, processed.mutex, control.mutex);
        collected.lastWS.assign(turbineCount, 0.0);
        collected.lastWS_t.assign(turbineCount, 0);
        collected.lastWD.assign(turbineCount, 0.0);
        collected.lastWD_t.assign(turbineCount, 0);
        collected.lastYawOffset.assign(turbineCount, 0.0);
        collected.lastYawOffset_t.assign(turbineCount, 0);
        collected.lastRPM.assign(turbineCount, 0.0);
        collected.lastRPM_t.assign(turbineCount, 0);
        collected.lastPower.assign(turbineCount, 0.0);
        collected.lastPower_t.assign(turbineCount, 0);
        collected.lastGenTorque.assign(turbineCount, 0.0);
        collected.lastGenTorque_t.assign(turbineCount, 0);
        collected.wsHistory = makeTurbineHistory<double>(turbineCount, CollectedData::historySampleCapacity);
        collected.wdHistory = makeTurbineHistory<double>(turbineCount, CollectedData::historySampleCapacity);
        collected.yawOffsetHistory = makeTurbineHistory<double>(turbineCount, CollectedData::historySampleCapacity);
        collected.rpmHistory = makeTurbineHistory<double>(turbineCount, CollectedData::historySampleCapacity);
        collected.powerHistory = makeTurbineHistory<double>(turbineCount, CollectedData::historySampleCapacity);
        collected.genTorqueHistory = makeTurbineHistory<double>(turbineCount, CollectedData::historySampleCapacity);
        collected.measuredPower.assign(turbineCount, 0.0);

        processed.availablePower.assign(turbineCount, 0.0);
        processed.filteredWindSpeeds.assign(turbineCount, 0.0);
        processed.filteredWindSpeedTimeMs.assign(turbineCount, 0);

        control.powerSetpoints.assign(turbineCount, -1.0f);
        control.yawSetpoints.assign(turbineCount, 0.0f);
        control.turbineEnabled.assign(turbineCount, 1);
        control.turbineController.assign(turbineCount, ControlData::controllerKomega2);
    }
};
