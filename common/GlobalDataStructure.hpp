#pragma once

#include <boost/circular_buffer.hpp>
#include <mutex>
#include <string>
#include <vector>

constexpr int N_TURBINES = 8;

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
    std::string statusMessage;

    // ── Raw Measurement Vectors ─────────────────────────────────────────────
    // Written by CommunicationTask
    // Read by SignalProcessingTask, ControlTask
    std::vector<double> lastWS = std::vector<double>(N_TURBINES, 0.0);  // m/s
    std::vector<double> lastWD = std::vector<double>(N_TURBINES, 0.0);  // degrees from north
    std::vector<double> lastRPM = std::vector<double>(N_TURBINES, 0.0); // revolutions per minute

    // ── Measurement history buffers (last N_hist readings per turbine) ────────
    // Written by CommunicationTask, read by SignalProcessingTask, ControlTask
    static constexpr int N_hist = 10;

    TurbineHistory<double> wsHistory  = makeTurbineHistory<double>(N_TURBINES, N_hist);
    TurbineHistory<double> wdHistory  = makeTurbineHistory<double>(N_TURBINES, N_hist);
    TurbineHistory<double> rpmHistory = makeTurbineHistory<double>(N_TURBINES, N_hist);
    TurbineHistory<double> powerHistory = makeTurbineHistory<double>(N_TURBINES, N_hist);


    // ── Processed Data
    std::vector<double> Power_i = std::vector<double>(N_TURBINES, 0.0); // instantaneous power per turbine in watts
    std::vector<double> Power_avg20 = std::vector<double>(N_TURBINES, 0.0); // 20-s moving average of power per turbine in watts
    std::vector<double> AvailablePower = std::vector<double>(N_TURBINES, 0.0); // available power per turbine in watts (from power curve)
    std::vector<double> rpm_i = std::vector<double>(N_TURBINES, 0.0); // instantaneous RPM per turbine
    std::vector<double> rpm_avg20 = std::vector<double>(N_TURBINES, 0.0); // 20-s moving average of RPM per turbine
    float glob_ws_i = 0.0f; // global instantaneous wind speed (e.g. farm-level average)
    float glob_ws_avg20 = 0.0f; // global 20-s moving average of wind speed
    float glob_wd_i = 0.0f; // global instantaneous wind direction (e.g. farm-level average)
    float glob_wd_avg20 = 0.0f; // global 20-s moving average of wind direction

    // ── Per-turbine setpoints (written by ControlTask, read by CommunicationTask)
    //    Sized to N_TURBINES; power in watts, yaw in degrees.
    std::vector<float> TurbinePowerSetpoints = std::vector<float>(N_TURBINES, -1.0f);
    std::vector<int>   TurbineYawSetpoints   = std::vector<int>  (N_TURBINES,  0);


    // -- Data from the grid operator
    float RequestedReferencePower = 0.0f; 
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

    GlobalDataStructure(const GlobalDataStructure&)             = delete;
    GlobalDataStructure& operator=(const GlobalDataStructure&)  = delete;

private:
    GlobalDataStructure() = default;

    std::mutex mutex_;
    GlobalData data_;
};
