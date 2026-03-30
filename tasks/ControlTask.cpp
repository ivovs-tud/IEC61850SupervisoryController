#include <iostream>

#include "ControlTask.hpp"
#include "common/GlobalDataStructure.hpp"


ControlTask::ControlTask(int numTurbines, std::chrono::milliseconds period)
    : PeriodicTask(period), numTurbines_(numTurbines)
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

    std::cout << "Using Wind Speed: " << glob_ws_i << " m/s, Wind Direction: " << glob_wd_i << " deg, to compute setpoints for requested reference power: " << powerSetpoint << " W\n";
    std::vector<float> power_sp = std::vector<float>(numTurbines_, -1.0);
    std::vector<int> yaw_sp = std::vector<int>(numTurbines_, 0);
    
    if (powerSetpoint < 0.0f) {
        // This means power maximization, i.e. yaw steering --> Use LUT
        yaw_sp = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90};
    } else {
        // This means power tracking --> For now we split equally accross all turbines,
        power_sp = std::vector<float>(numTurbines_, static_cast<float>(powerSetpoint / numTurbines_));
    }

    std::cout << "ControlTask: Computed power setpoints: ";
    for (const auto& sp : power_sp) {
        std::cout << sp << " ";
    }
    std::cout << "\nControlTask: Computed yaw setpoints: ";
    for (const auto& sp : yaw_sp) {
        std::cout << sp << " ";
    }
    std::cout << "\n";

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
    std::cout << "ControlTask: Stopped\n";
}