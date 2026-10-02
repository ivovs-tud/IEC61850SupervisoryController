#include "sc/tasks/MonitoringTask.hpp"

#include "sc/model/SharedData.hpp"
#include "sc/runtime/Time.hpp"

#include <stdexcept>
#include <vector>

namespace {

template <typename HistoryType>
std::vector<double> copyHistory(const HistoryType& history) {
    return {history.begin(), history.end()};
}

std::size_t checkedTurbineCount(int turbineCount) {
    if (turbineCount <= 0) throw std::invalid_argument("MonitoringTask requires at least one turbine");
    return static_cast<std::size_t>(turbineCount);
}

sc::application::MonitoringInput copyMonitoringInput(const SharedData& data, int turbineCount,
                                                     uint64_t currentTimeMs) {
    sc::application::MonitoringInput input;
    input.currentTimeMs = currentTimeMs;
    input.turbines.resize(static_cast<std::size_t>(turbineCount));

    {
        std::lock_guard<std::mutex> lock(data.collected.mutex);
        for (int index = 0; index < turbineCount; ++index) {
            auto& target = input.turbines[static_cast<std::size_t>(index)];
            // Raw local wind signals remain inputs to telemetry-consistency detectors.
            target.windSpeed = data.collected.lastWS[index];
            target.windSpeedTimeMs = data.collected.lastWS_t[index];
            target.windDirection = data.collected.lastWD[index];
            target.windDirectionTimeMs = data.collected.lastWD_t[index];
            target.yaw = data.collected.lastYawOffset[index];
            target.yawTimeMs = data.collected.lastYawOffset_t[index];
            target.rotorSpeed = data.collected.lastRPM[index];
            target.rotorSpeedTimeMs = data.collected.lastRPM_t[index];
            target.power = data.collected.lastPower[index];
            target.powerTimeMs = data.collected.lastPower_t[index];
            target.generatorTorque = data.collected.lastGenTorque[index];
            target.generatorTorqueTimeMs = data.collected.lastGenTorque_t[index];
            target.windSpeedHistory = copyHistory(data.collected.wsHistory[index]);
            target.windDirectionHistory = copyHistory(data.collected.wdHistory[index]);
            target.rotorSpeedHistory = copyHistory(data.collected.rpmHistory[index]);
            target.powerHistory = copyHistory(data.collected.powerHistory[index]);
            target.generatorTorqueHistory = copyHistory(data.collected.genTorqueHistory[index]);
        }
    }

    {
        std::lock_guard<std::mutex> lock(data.processed.mutex);
        input.connectedTurbines = data.processed.connectedTurbines;
        input.farmWindSpeed = data.processed.windSpeed;
        input.farmWindDirection = data.processed.windDirection;
        input.measuredTotalPowerHistory = copyHistory(data.processed.measuredTotalPowerHistory);
        if (data.processed.filteredWindSpeeds.size() != input.turbines.size() ||
            data.processed.filteredWindSpeedTimeMs.size() != input.turbines.size()) {
            throw std::logic_error("processed wind-speed count does not match configured turbines");
        }
        for (std::size_t index = 0; index < input.turbines.size(); ++index) {
            input.turbines[index].filteredWindSpeed = data.processed.filteredWindSpeeds[index];
            input.turbines[index].filteredWindSpeedTimeMs = data.processed.filteredWindSpeedTimeMs[index];
        }
    }

    {
        std::lock_guard<std::mutex> lock(data.control.mutex);
        for (int index = 0; index < turbineCount; ++index) {
            auto& target = input.turbines[static_cast<std::size_t>(index)];
            target.powerSetpoint = data.control.powerSetpoints[index];
            target.yawSetpoint = data.control.yawSetpoints[index];
            target.maximumPowerMode = data.control.turbineController[index] == ControlData::controllerKomega2;
            target.commandedOff = data.control.turbineEnabled[index] == 0 ||
                                  data.control.turbineController[index] == ControlData::controllerShutdown;
        }
    }

    return input;
}

} // namespace

MonitoringTask::MonitoringTask(std::chrono::milliseconds period, int numTurbines,
                               bool alarmAcknowledgementEnabled)
    : PeriodicTask(period), numTurbines_(numTurbines),
      alarmAcknowledgementEnabled_(alarmAcknowledgementEnabled),
      detectors_(checkedTurbineCount(numTurbines)) {}

void MonitoringTask::execute() {
    auto& data = SharedData::instance();
    {
        std::lock_guard<std::mutex> lock(data.interface.mutex);
        if (!data.interface.systemRunning) return;
    }

    const uint64_t currentTimeMs = getCurrentTimeMs();
    bool acknowledgementRequested = false;
    if (alarmAcknowledgementEnabled_) {
        std::lock_guard<std::mutex> lock(data.control.mutex);
        acknowledgementRequested = data.control.alarmAcknowledgementRequested;
        data.control.alarmAcknowledgementRequested = false;
    }
    const auto result = detectors_.evaluate(copyMonitoringInput(data, numTurbines_, currentTimeMs));

    std::lock_guard<std::mutex> lock(data.monitoring.mutex);
    auto& alarms = data.monitoring;
    if (alarmAcknowledgementEnabled_) {
        if (acknowledgementRequested) alarmStates_.acknowledge();
        alarmStates_.update(result);
        alarms.alarmWRecMeas = alarmStates_.visible(sc::application::AlarmType::PowerGeneratedVsReceived);
        alarms.alarmPowerExpected = alarmStates_.visible(sc::application::AlarmType::MeasuredPowerVsExpected);
        alarms.alarmOrientationMisalign = alarmStates_.visible(sc::application::AlarmType::OrientationMisalignment);
        alarms.alarmWTorqueRotSpd = alarmStates_.visible(sc::application::AlarmType::PowerTorqueRotorSpeed);
        alarms.alarmHorWdDir = alarmStates_.visible(sc::application::AlarmType::WindDirection);
        alarms.alarmHorWdDirChg = alarmStates_.visible(sc::application::AlarmType::WindDirectionChange);
        alarms.alarmHorWdSpdChg = alarmStates_.visible(sc::application::AlarmType::WindSpeedChange);
        alarms.alarmTelemetryFreezeReplay = alarmStates_.visible(sc::application::AlarmType::TelemetryFreeze);
        alarms.alarmDrivetrainUnderResponse = alarmStates_.visible(sc::application::AlarmType::DrivetrainUnderResponse);
        alarms.alarmStaticBounds = alarmStates_.visible(sc::application::AlarmType::StaticTelemetryBounds);
        alarms.alarmFleetPeerOutlier = alarmStates_.visible(sc::application::AlarmType::FleetPeerOutlier);
        return;
    }

    // Preserve the legacy clear interval until operator acknowledgement semantics are chosen.
    if (currentTimeMs - lastAlarmResetMs_ >= 3000) {
        alarms.alarmWRecMeas = false;
        alarms.alarmPowerExpected = false;
        alarms.alarmOrientationMisalign = false;
        alarms.alarmWTorqueRotSpd = false;
        alarms.alarmHorWdDir = false;
        alarms.alarmHorWdDirChg = false;
        alarms.alarmHorWdSpdChg = false;
        alarms.alarmTelemetryFreezeReplay = false;
        alarms.alarmDrivetrainUnderResponse = false;
        alarms.alarmStaticBounds = false;
        alarms.alarmFleetPeerOutlier = false;
        lastAlarmResetMs_ = currentTimeMs;
    }

    alarms.alarmWRecMeas |= result.isActive(sc::application::AlarmType::PowerGeneratedVsReceived);
    alarms.alarmPowerExpected |= result.isActive(sc::application::AlarmType::MeasuredPowerVsExpected);
    alarms.alarmOrientationMisalign |= result.isActive(sc::application::AlarmType::OrientationMisalignment);
    alarms.alarmWTorqueRotSpd |= result.isActive(sc::application::AlarmType::PowerTorqueRotorSpeed);
    alarms.alarmHorWdDir |= result.isActive(sc::application::AlarmType::WindDirection);
    alarms.alarmHorWdDirChg |= result.isActive(sc::application::AlarmType::WindDirectionChange);
    alarms.alarmHorWdSpdChg |= result.isActive(sc::application::AlarmType::WindSpeedChange);
    alarms.alarmTelemetryFreezeReplay |= result.isActive(sc::application::AlarmType::TelemetryFreeze);
    alarms.alarmDrivetrainUnderResponse |= result.isActive(sc::application::AlarmType::DrivetrainUnderResponse);
    alarms.alarmStaticBounds |= result.isActive(sc::application::AlarmType::StaticTelemetryBounds);
    alarms.alarmFleetPeerOutlier |= result.isActive(sc::application::AlarmType::FleetPeerOutlier);
}
