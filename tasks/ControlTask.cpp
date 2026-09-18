#include <iostream>
#include <sstream>
#include <stdexcept>

#include "ControlTask.hpp"
#include "common/SharedData.hpp"
#include "common/config.hpp"
#include "sc/application/ControlCalculation.hpp"

ControlTask::ControlTask(Config config)
    : PeriodicTask(config.period), yawLut_(config.yawLutCsvPath), numTurbines_(config.numTurbines) {}

void ControlTask::execute() {
    sc::application::ControlInputs inputs;
    inputs.turbineCount = numTurbines_;

    auto& data = SharedData::instance();
    {
        std::lock_guard<std::mutex> lock(data.control.mutex);
        inputs.requestedReferencePower = data.control.requestedPower;
        inputs.yawSteeringEnabled = data.control.yawSteeringEnabled;
    }
    {
        std::lock_guard<std::mutex> lock(data.processed.mutex);
        inputs.windSpeed = data.processed.windSpeed;
        inputs.windDirection = data.processed.windDirection;
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

    std::lock_guard<std::mutex> lock(data.control.mutex);
    if (setpoints.turbinePower.size() != data.control.powerSetpoints.size() ||
        setpoints.turbineYaw.size() != data.control.yawSetpoints.size()) {
        throw std::logic_error("control setpoint count does not match configured turbines");
    }
    data.control.powerSetpoints = setpoints.turbinePower;
    data.control.yawSetpoints = setpoints.turbineYaw;
}

void ControlTask::onStop() {
    CONTROL_LOG_V1("Stopped");
}
