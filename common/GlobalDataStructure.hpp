#pragma once

#include <algorithm>
#include <boost/circular_buffer.hpp>
#include <mutex>
#include <string>
#include <vector>

constexpr int N_TURBINES = 9;

// Per-turbine circular-buffer history: one buffer per turbine, each holding
// up to 'capacity' readings of type T.  Push new values with push_back().
template <typename T>
using History = boost::circular_buffer<T>;

template <typename T>
using TurbineHistory = std::vector<History<T>>;

template <typename T>
inline TurbineHistory<T> makeTurbineHistory(int numTurbines, int capacity)
{
    return TurbineHistory<T>(numTurbines, History<T>(capacity));
}

// ---------------------------------------------------------------------------
// Shared operational data – all fields are placeholders.
// Usage pattern:
//   std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
//   GlobalDataStructure::instance().data().measuredVoltage = 230.0;
// ---------------------------------------------------------------------------
struct GlobalData
{
    // ── System state ────────────────────────────────────────────────────────
    bool        systemRunning  {false};
    int         connectedTurbines {0};
    std::string statusMessage;

    // ── Raw Measurement Vectors ─────────────────────────────────────────────
    // Written by CommunicationTask
    // Read by SignalProcessingTask, ControlTask
    std::vector<double> lastWS = std::vector<double>(N_TURBINES, 0.0);  // m/s
    std::vector<uint64_t> lastWS_t = std::vector<uint64_t>(N_TURBINES, 0); // timestamp of the last received wind speed measurement (UNIX time in seconds)

    std::vector<double> lastWD = std::vector<double>(N_TURBINES, 0.0);  // degrees from north
    std::vector<uint64_t> lastWD_t = std::vector<uint64_t>(N_TURBINES, 0); // timestamp of the last received wind direction measurement (UNIX time in seconds)

    std::vector<double> lastYawOffset = std::vector<double>(N_TURBINES, 0.0); // degrees
    std::vector<uint64_t> lastYawOffset_t = std::vector<uint64_t>(N_TURBINES, 0); // timestamp of the last received yaw offset measurement (UNIX time in seconds)

    std::vector<double> lastRPM = std::vector<double>(N_TURBINES, 0.0); // revolutions per minute
    std::vector<uint64_t> lastRPM_t = std::vector<uint64_t>(N_TURBINES, 0); // timestamp of the last received RPM measurement (UNIX time in seconds)

    std::vector<double> lastPower = std::vector<double>(N_TURBINES, 0.0);
    std::vector<uint64_t> lastPower_t = std::vector<uint64_t>(N_TURBINES, 0); // timestamp of the last received power measurement (UNIX time in seconds)

    std::vector<double> lastGenTorque = std::vector<double>(N_TURBINES, 0.0);
    std::vector<uint64_t> lastGenTorque_t = std::vector<uint64_t>(N_TURBINES, 0); // timestamp of the last received gentorque measurement (UNIX time in seconds)
    // ── Measurement history buffers (last N_hist readings per turbine) ────────
    // Written by CommunicationTask, read by SignalProcessingTask, ControlTask
    static constexpr int N_hist = 10;

    TurbineHistory<double> wsHistory  = makeTurbineHistory<double>(N_TURBINES, N_hist);
    TurbineHistory<double> wdHistory  = makeTurbineHistory<double>(N_TURBINES, N_hist);
    TurbineHistory<double> yawOffsetHistory = makeTurbineHistory<double>(N_TURBINES, N_hist);
    TurbineHistory<double> rpmHistory = makeTurbineHistory<double>(N_TURBINES, N_hist);
    TurbineHistory<double> powerHistory = makeTurbineHistory<double>(N_TURBINES, N_hist);
    TurbineHistory<double> genTorqueHistory = makeTurbineHistory<double>(N_TURBINES, N_hist);

    // Values stored to compute other quantities which are measured locally. Crucially these are not used directly
	std::vector<double> _W = std::vector<double>(N_TURBINES, 0.0); 
    History<double> Wtotal_meas = History<double>(N_hist);


    // ── Processed Data
    std::vector<double> Power_i = std::vector<double>(N_TURBINES, 0.0); // instantaneous power per turbine in watts
    std::vector<double> Power_avg20 = std::vector<double>(N_TURBINES, 0.0); // 20-s moving average of power per turbine in watts
    std::vector<double> AvailablePower = std::vector<double>(N_TURBINES, 0.0); // available power per turbine in watts (from power curve)
    double TotalPower_recv = 0.0f;
    std::vector<double> rpm_i = std::vector<double>(N_TURBINES, 0.0); // instantaneous RPM per turbine
    std::vector<double> rpm_avg20 = std::vector<double>(N_TURBINES, 0.0); // 20-s moving average of RPM per turbine
    float glob_ws_i = 0.0f; // global instantaneous wind speed (e.g. farm-level average)
    float glob_ws_avg20 = 0.0f; // global 20-s moving average of wind speed
    float glob_wd_i = 270.0f; // global instantaneous wind direction (e.g. farm-level average)
    float glob_wd_avg20 = 0.0f; // global 20-s moving average of wind direction

    // ── Per-turbine setpoints (written by ControlTask, read by CommunicationTask)
    //    Sized to N_TURBINES; power in watts, yaw in degrees.
    std::vector<float> TurbinePowerSetpoints = std::vector<float>(N_TURBINES, -1.0f);
    std::vector<float> TurbineYawSetpoints   = std::vector<float>(N_TURBINES,  0.0f);


    // -- Data from the grid operator
    float RequestedReferencePower = -1.0f; 

    // -- HMI control + annunciator state
    static constexpr uint32_t turbineControllerKomega2 = 1;
    static constexpr uint32_t turbineControllerDownregulation = 2;
    static constexpr uint32_t turbineControllerShutdown = 3;

    // TurbineController values:
    //   1 = Komega^2, 2 = down-regulation, 3 = shutdown
	bool yawSteeringEnabled = false;
	std::string yawSteeringCommandName = "Yaw Steering";
	std::vector<uint32_t> enableTurbine = std::vector<uint32_t>(N_TURBINES, 1); // Per-turbine enable/disable flags (1 = enabled, 0 = disabled)
	std::vector<uint32_t> TurbineController = std::vector<uint32_t>(N_TURBINES, 1); // Per-turbine operation mode (minimum is 1)
    int attackTapEnabled = 0;
    int attackTapAvailable = 0;
    int attackFdiEnabled = 0;
    int attackFdiAvailable = 0;
    std::vector<std::string> attackFdiSignals;
    

    // -- Monitoring Task Related ---

    bool alarmWRecMeas = false;
    bool alarmOrientationMisalign = false;
    bool alarmWTorqueRotSpd = false;
    bool alarmPowerExpected = false;
    bool alarmHorWdDir = false;
    bool alarmHorWdDirChg = false;
    bool alarmHorWdSpdChg = false;
    bool alarmTelemetryFreezeReplay = false;
    bool alarmDrivetrainUnderResponse = false;
    bool alarmStaticBounds = false;
    bool alarmFleetPeerOutlier = false;
    std::vector<float> orientations = std::vector<float>(N_TURBINES, 0.0);



    // Fields for property coordination with the simulator
    bool simStarted = false;
    bool simConfigured = false;
    std::string simTeamName;
    int simScenario = 0;

    // Turbine Parameter Values
    static constexpr const char* turbineModelName = "NREL5MW";
    static constexpr double airDensity = 1.225; // kg/m^3
    static constexpr double rotorDiameter = 126.0; // m
    static constexpr double optimalPowerCoefficient = 0.482;
    static constexpr double optimalTipSpeedRatio = 7.55;
    static constexpr double rotorInertia = 4e6; // kg m^2
    static constexpr double gearboxRatio = 97.0;
    static constexpr double generatorEfficiency = 0.944;
    static constexpr double ratedPower = 5e6; // W
    static constexpr double cutInWindSpeed = 3.0; // m/s
    static constexpr double ratedWindSpeed = 11.4; // m/s
    static constexpr double cutOutWindSpeed = 25.0; // m/s
    static constexpr double ratedRotorSpeed = 12.1; // rpm
    static constexpr double minimumRotorSpeed = 0.722; // rad/s
    static constexpr double pitchRate = 10.0; // deg/s
    static constexpr double brakeTorque = 28116.2; // Nm
    static constexpr double yawingRate = 5.0; // deg/s
    static constexpr double ratedTorque = 31465000.0; // Nm
    static constexpr double maximumGeneratorTorque = 47402.91; // Nm
    

    void resetForNewRunFields(const std::string& teamName,
                              int scenarioId,
                              int turbineControllerId)
    {
        systemRunning = false;
        connectedTurbines = 0;
        statusMessage.clear();

        std::fill(lastWS.begin(), lastWS.end(), 0.0);
        std::fill(lastWS_t.begin(), lastWS_t.end(), 0);
        std::fill(lastWD.begin(), lastWD.end(), 0.0);
        std::fill(lastWD_t.begin(), lastWD_t.end(), 0);
        std::fill(lastYawOffset.begin(), lastYawOffset.end(), 0.0);
        std::fill(lastYawOffset_t.begin(), lastYawOffset_t.end(), 0);
        std::fill(lastRPM.begin(), lastRPM.end(), 0.0);
        std::fill(lastRPM_t.begin(), lastRPM_t.end(), 0);
        std::fill(lastPower.begin(), lastPower.end(), 0.0);
        std::fill(lastPower_t.begin(), lastPower_t.end(), 0);
        std::fill(lastGenTorque.begin(), lastGenTorque.end(), 0.0);
        std::fill(lastGenTorque_t.begin(), lastGenTorque_t.end(), 0);

        for (auto& h : wsHistory)
            h.clear();
        for (auto& h : wdHistory)
            h.clear();
        for (auto& h : yawOffsetHistory)
            h.clear();
        for (auto& h : rpmHistory)
            h.clear();
        for (auto& h : powerHistory)
            h.clear();

        std::fill(Power_i.begin(), Power_i.end(), 0.0);
        std::fill(Power_avg20.begin(), Power_avg20.end(), 0.0);
        std::fill(AvailablePower.begin(), AvailablePower.end(), 0.0);
        std::fill(rpm_i.begin(), rpm_i.end(), 0.0);
        std::fill(rpm_avg20.begin(), rpm_avg20.end(), 0.0);
        glob_ws_i = 0.0f;
        glob_ws_avg20 = 0.0f;
        glob_wd_i = 0.0f;
        glob_wd_avg20 = 0.0f;

        std::fill(TurbinePowerSetpoints.begin(), TurbinePowerSetpoints.end(), 0.0f);
        std::fill(TurbineYawSetpoints.begin(), TurbineYawSetpoints.end(), 0);

        RequestedReferencePower = -1.0f;

        yawSteeringEnabled = false;
        yawSteeringCommandName = "Yaw Steering";
        attackTapEnabled = 0;
        attackTapAvailable = 0;
        attackFdiEnabled = 0;
        attackFdiAvailable = 0;
        attackFdiSignals.clear();

        alarmWRecMeas = false;
        alarmOrientationMisalign = false;
        alarmWTorqueRotSpd = false;
        alarmPowerExpected = false;
        alarmHorWdDir = false;
        alarmHorWdDirChg = false;
        alarmHorWdSpdChg = false;
        alarmTelemetryFreezeReplay = false;
        alarmDrivetrainUnderResponse = false;
        alarmStaticBounds = false;
        alarmFleetPeerOutlier = false;

        simStarted = false;
        simConfigured = true;
        simTeamName = teamName;
        simScenario = scenarioId;
        //TurbineController = turbineControllerId;
    }
};

// Singleton providing mutex-protected access to the shared GlobalData struct.
class GlobalDataStructure
{
public:
    static GlobalDataStructure& instance()
    {
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
