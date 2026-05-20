#include <iostream>
#include <sstream>
#include <cmath>

#include "ControlTask.hpp"
#include "common/config.hpp"
#include "common/GlobalDataStructure.hpp"


ControlTask::ControlTask(Config config)
    : PeriodicTask(config.period), numTurbines_(config.numTurbines), yawLut_(config.yawLutCsvPath)
{
    // TODO: initialise control algorithm state
}

void ControlTask::execute()
{
    /**
     * @brief The execution loop is simple: 
     * The command from the operator is received, and does either power tracking or yaw steering
     * It is assumed all relevant operational data used for this (e.g. wind speed and direction) has been
     * pre-processed and/or determined in the SignalProcessingTask and is available in the GlobalDataStructure.
     * 
     */
    // Read Data from GlobalDataStructure
    float powerSetpoint = 0.0f;
    float glob_ws_i = 0.0f;
    float glob_wd_i = 0.0f;
    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        powerSetpoint = GlobalDataStructure::instance().data().RequestedReferencePower;
        glob_ws_i = GlobalDataStructure::instance().data().glob_ws_i;
        glob_wd_i = GlobalDataStructure::instance().data().glob_wd_i;
    }

    // (void)glob_ws_i;
    // (void)glob_wd_i;

    CONTROL_LOG_V2("Using Wind Speed: " << glob_ws_i << " m/s, Wind Direction: " << glob_wd_i
                  << " deg, to compute setpoints for requested reference power: " << powerSetpoint << " W");
    std::vector<float> power_sp = std::vector<float>(numTurbines_, -1.0);
    std::vector<int> yaw_sp = std::vector<int>(numTurbines_, 0);
    
    if (powerSetpoint < 0.0f) {
        // This means power maximization, i.e. yaw steering --> Use LUT and round to nearest int
        
        const auto yaw_sp_float = yawLut_.lookup(glob_ws_i, glob_wd_i);
        yaw_sp.clear();
        yaw_sp.reserve(yaw_sp_float.size());
        for (const float value : yaw_sp_float) {
            yaw_sp.push_back(static_cast<int>(std::lround(value)));
        }
        
    } else {
        // This means power tracking --> For now we split equally accross all turbines,
        power_sp = std::vector<float>(numTurbines_, static_cast<float>(powerSetpoint / numTurbines_));
    }

#if SC_LOG_LEVEL_CONTROL >= 2
    std::ostringstream powerLine;
    for (const auto& sp : power_sp) {
        powerLine << sp << " ";
    }
    CONTROL_LOG_V1("Computed power setpoints: " << powerLine.str());

    std::ostringstream yawLine;
    for (const auto& sp : yaw_sp) {
        yawLine << sp << " ";
    }
    CONTROL_LOG_V1("Computed yaw setpoints: " << yawLine.str());
#endif

    // Next, we push this to the global data structure, to be send automatically to the turbines by the CommunicationTask.
    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        for (int i = 0; i < numTurbines_; ++i) {
            GlobalDataStructure::instance().data().TurbinePowerSetpoints[i] = power_sp[i];
            GlobalDataStructure::instance().data().TurbineYawSetpoints[i] = yaw_sp[i];
        }
    }
}


void ControlTask::onStop()
{
    CONTROL_LOG_V1("Stopped");
}