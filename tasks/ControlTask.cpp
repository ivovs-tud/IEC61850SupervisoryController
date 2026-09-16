#include <iostream>
#include <sstream>
#include <algorithm>

#include "ControlTask.hpp"
#include "common/config.hpp"
#include "common/GlobalDataStructure.hpp"
#include "sc/application/ControlCalculation.hpp"


ControlTask::ControlTask(Config config)
    : PeriodicTask(config.period), yawLut_(config.yawLutCsvPath), numTurbines_(config.numTurbines)
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
    sc::application::ControlInputs inputs;
    inputs.turbineCount = numTurbines_;

    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        const auto& gds = GlobalDataStructure::instance().data();
        inputs.requestedReferencePower = gds.RequestedReferencePower;
        inputs.windSpeed = gds.glob_ws_i;
        inputs.windDirection = gds.glob_wd_i;
    }

    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        inputs.yawSteeringEnabled = GlobalDataStructure::instance().data().yawSteeringEnabled;
    }

    CONTROL_LOG_V2("Using Wind Speed: " << inputs.windSpeed << " m/s, Wind Direction: " << inputs.windDirection
                  << " deg, to compute setpoints for requested reference power: "
                  << inputs.requestedReferencePower << " W");
    const auto setpoints = sc::application::calculateControlSetpoints(inputs, yawLut_);

#if SC_LOG_LEVEL_CONTROL >= 2
    std::ostringstream powerLine;
    for (const auto& sp : setpoints.turbinePower) {
        powerLine << sp << " ";
    }
    CONTROL_LOG_V1("Computed power setpoints: " << powerLine.str());

    std::ostringstream yawLine;
    for (const auto& sp : setpoints.turbineYaw) {
        yawLine << sp << " ";
    }
    CONTROL_LOG_V1("Computed yaw setpoints: " << yawLine.str());
#endif

    // Next, we push this to the global data structure, to be send automatically to the turbines by the CommunicationTask.
    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        auto& gds = GlobalDataStructure::instance().data();
        const int n = std::min({numTurbines_,
                                static_cast<int>(gds.TurbinePowerSetpoints.size()),
                                static_cast<int>(gds.TurbineYawSetpoints.size()),
                                static_cast<int>(setpoints.turbinePower.size()),
                                static_cast<int>(setpoints.turbineYaw.size())});
        for (int i = 0; i < n; ++i) {
            gds.TurbinePowerSetpoints[i] = setpoints.turbinePower[i];
            gds.TurbineYawSetpoints[i] = static_cast<float>(setpoints.turbineYaw[i]);
        }
    }
}


void ControlTask::onStop()
{
    CONTROL_LOG_V1("Stopped");
}
