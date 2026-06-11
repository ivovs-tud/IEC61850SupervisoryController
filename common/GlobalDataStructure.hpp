#pragma once

#include <algorithm>
#include <boost/circular_buffer.hpp>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

constexpr int kMaxTurbines = 9;

/**
 * Fixed-size history buffer used for sampled controller signals.
 *
 * Each turbine owns one circular buffer. New samples overwrite the oldest
 * samples after `kHistoryCapacity`, which keeps the shared state bounded even
 * when communication runs for long experiments.
 */
template <typename T>
using History = boost::circular_buffer<T>;

template <typename T>
using TurbineHistory = std::vector<History<T>>;

template <typename T>
inline TurbineHistory<T> makeTurbineHistory(int numTurbines, int capacity) {
    return TurbineHistory<T>(numTurbines, History<T>(capacity));
}

/**
 * Shared controller state exchanged by periodic tasks.
 *
 * All per-turbine vectors are sized to `kMaxTurbines` and indexed with
 * zero-based indices. External turbine IDs are one-based, so communication
 * code must convert with `turbineId - 1` after validating the ID.
 *
 * Access must be protected with `GlobalDataStructure::mutex()` for both reads
 * and writes:
 *
 *     std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
 *     auto& data = GlobalDataStructure::instance().data();
 */
struct GlobalData {
    // Runtime state exported to UI and monitoring.
    bool        systemRunning  {false};
    std::string statusMessage;

    // Latest raw turbine measurements. Written by communication, consumed by
    // signal processing, control, monitoring, and HMI.
    std::vector<double> lastWindSpeed = std::vector<double>(kMaxTurbines, 0.0);         // m/s
    std::vector<uint64_t> lastWindSpeedTimestampMs = std::vector<uint64_t>(kMaxTurbines, 0);

    std::vector<double> lastWindDirection = std::vector<double>(kMaxTurbines, 0.0);     // deg from north
    std::vector<uint64_t> lastWindDirectionTimestampMs = std::vector<uint64_t>(kMaxTurbines, 0);

    std::vector<double> lastYawOffset = std::vector<double>(kMaxTurbines, 0.0);          // deg
    std::vector<uint64_t> lastYawOffsetTimestampMs = std::vector<uint64_t>(kMaxTurbines, 0);

    std::vector<double> lastRotorSpeed = std::vector<double>(kMaxTurbines, 0.0);         // RPM
    std::vector<uint64_t> lastRotorSpeedTimestampMs = std::vector<uint64_t>(kMaxTurbines, 0);

    std::vector<double> lastPower = std::vector<double>(kMaxTurbines, 0.0);              // W
    std::vector<uint64_t> lastPowerTimestampMs = std::vector<uint64_t>(kMaxTurbines, 0);

    std::vector<double> lastGeneratorTorque = std::vector<double>(kMaxTurbines, 0.0);    // Nm
    std::vector<uint64_t> lastGeneratorTorqueTimestampMs = std::vector<uint64_t>(kMaxTurbines, 0);

    // Short histories support step-change and moving-window checks without
    // growing memory use over long simulations.
    static constexpr int kHistoryCapacity = 10;

    TurbineHistory<double> windSpeedHistory = makeTurbineHistory<double>(kMaxTurbines, kHistoryCapacity);
    TurbineHistory<double> windDirectionHistory  = makeTurbineHistory<double>(kMaxTurbines, kHistoryCapacity);
    TurbineHistory<double> yawOffsetHistory = makeTurbineHistory<double>(kMaxTurbines, kHistoryCapacity);
    TurbineHistory<double> rotorSpeedHistory = makeTurbineHistory<double>(kMaxTurbines, kHistoryCapacity);
    TurbineHistory<double> powerHistory = makeTurbineHistory<double>(kMaxTurbines, kHistoryCapacity);
    TurbineHistory<double> generatorTorqueHistory = makeTurbineHistory<double>(kMaxTurbines, kHistoryCapacity);

    // Local metering path: values come from power measurements and are used to
    // compare received farm power against independently accumulated power.
    std::vector<double> localMeasuredPower = std::vector<double>(kMaxTurbines, 0.0);
    History<double> measuredTotalPowerHistory = History<double>(kHistoryCapacity);

    // Processed farm values derived from raw measurements.
    std::vector<double> turbinePower = std::vector<double>(kMaxTurbines, 0.0);             // W
    std::vector<double> turbinePowerAverage20s = std::vector<double>(kMaxTurbines, 0.0);   // W
    std::vector<double> availablePower = std::vector<double>(kMaxTurbines, 0.0);           // W, from power curve
    double receivedTotalPower = 0.0f;                                                      // W
    std::vector<double> turbineRotorSpeed = std::vector<double>(kMaxTurbines, 0.0);        // RPM
    std::vector<double> turbineRotorSpeedAverage20s = std::vector<double>(kMaxTurbines, 0.0);
    float farmWindSpeed = 0.0f;                                                           // m/s
    float farmWindSpeedAverage20s = 0.0f;                                                  // m/s
    float farmWindDirection = 0.0f;                                                        // deg
    float farmWindDirectionAverage20s = 0.0f;                                             // deg

    // Setpoints written by ControlTask and transmitted by IECCommunicator.
    std::vector<float> turbinePowerSetpoints = std::vector<float>(kMaxTurbines, -1.0f);
    std::vector<float> turbineYawSetpoints   = std::vector<float>(kMaxTurbines,  0.0f);

    // Grid-operator command. Negative values mean "no active request yet".
    float requestedReferencePower = -1.0f;

    // HMI controls and turbine command state.
    bool yawSteeringEnabled = false;
    std::string yawSteeringCommandName = "Yaw Steering";
    std::vector<uint32_t> turbineEnabled = std::vector<uint32_t>(kMaxTurbines, 1);         // 1 = enabled, 0 = disabled
    std::vector<uint32_t> turbineControllerModes = std::vector<uint32_t>(kMaxTurbines, 1); // controller ID sent over IEC

    // Monitoring latches. MonitoringTask clears these periodically; consumers
    // should treat them as recent alarms rather than permanent fault history.
    bool alarmPowerReceivedMismatch = false;
    bool alarmOrientationMisalignment = false;
    bool alarmPowerTorqueRotorSpeedMismatch = false;
    bool alarmWindDirectionMismatch = false;
    bool alarmWindDirectionChange = false;
    bool alarmWindSpeedChange = false;
    std::vector<float> orientations = std::vector<float>(kMaxTurbines, 0.0);

    // Scenario handshake with the simulator/test harness.
    bool simulationStarted = false;
    bool simulationConfigured = false;
    std::string simulationTeamName;
    int simulationScenarioId = 0;

    /**
     * Reset run-specific state when the simulator announces a new scenario.
     *
     * This intentionally preserves vector capacity and static configuration
     * while clearing values that would otherwise leak across scenarios.
     */
    void resetForNewRunFields(const std::string& teamName, int scenarioId, int turbineControllerId) {
        systemRunning = false;
        statusMessage.clear();

        std::fill(lastWindSpeed.begin(), lastWindSpeed.end(), 0.0);
        std::fill(lastWindDirection.begin(), lastWindDirection.end(), 0.0);
        std::fill(lastYawOffset.begin(), lastYawOffset.end(), 0.0);
        std::fill(lastRotorSpeed.begin(), lastRotorSpeed.end(), 0.0);

        for (auto& h : windSpeedHistory) {
            h.clear();
        }
        for (auto& h : windDirectionHistory) {
            h.clear();
        }
        for (auto& h : yawOffsetHistory) {
            h.clear();
        }
        for (auto& h : rotorSpeedHistory) {
            h.clear();
        }
        for (auto& h : powerHistory) {
            h.clear();
        }

        std::fill(turbinePower.begin(), turbinePower.end(), 0.0);
        std::fill(turbinePowerAverage20s.begin(), turbinePowerAverage20s.end(), 0.0);
        std::fill(availablePower.begin(), availablePower.end(), 0.0);
        std::fill(turbineRotorSpeed.begin(), turbineRotorSpeed.end(), 0.0);
        std::fill(turbineRotorSpeedAverage20s.begin(), turbineRotorSpeedAverage20s.end(), 0.0);
        farmWindSpeed = 0.0f;
        farmWindSpeedAverage20s = 0.0f;
        farmWindDirection = 0.0f;
        farmWindDirectionAverage20s = 0.0f;

        std::fill(turbinePowerSetpoints.begin(), turbinePowerSetpoints.end(), 0.0f);
        std::fill(turbineYawSetpoints.begin(), turbineYawSetpoints.end(), 0);

        requestedReferencePower = -1.0f;

        yawSteeringEnabled = false;
        yawSteeringCommandName = "Yaw Steering";

        alarmPowerReceivedMismatch = false;
        alarmOrientationMisalignment = false;
        alarmPowerTorqueRotorSpeedMismatch = false;
        alarmWindDirectionMismatch = false;
        alarmWindDirectionChange = false;
        alarmWindSpeedChange = false;

        simulationStarted = false;
        simulationConfigured = true;
        simulationTeamName = teamName;
        simulationScenarioId = scenarioId;
        std::fill(turbineControllerModes.begin(), turbineControllerModes.end(), static_cast<uint32_t>(turbineControllerId));
    }
};

// Singleton providing mutex-protected access to the shared GlobalData struct.
class GlobalDataStructure {
public:
    static GlobalDataStructure& instance() {
        static GlobalDataStructure inst;
        return inst;
    }

    std::mutex& mutex() { return mutex_; }
    GlobalData& data()  { return data_;  }

    void resetForNewRun(const std::string& teamName, int scenarioId, int turbineControllerId) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.resetForNewRunFields(teamName, scenarioId, turbineControllerId);
    }

    GlobalDataStructure(const GlobalDataStructure&)             = delete;
    GlobalDataStructure& operator=(const GlobalDataStructure&)  = delete;

private:
    GlobalDataStructure() = default;

    std::mutex mutex_;
    GlobalData data_;
};
