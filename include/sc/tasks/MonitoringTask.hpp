#pragma once

#include "sc/runtime/PeriodicTask.hpp"
#include "sc/model/SharedData.hpp"
#include "sc/application/Monitoring.hpp"

#include <chrono>
#include <cstdint>

class MonitoringTask : public PeriodicTask {
public:
    explicit MonitoringTask(std::chrono::milliseconds period = std::chrono::milliseconds(50),
                            int numTurbines = static_cast<int>(DEFAULT_TURBINE_COUNT),
                            bool alarmAcknowledgementEnabled = false);
    ~MonitoringTask() override { stop(); }

protected:
    void execute() override;

private:
    // GOOSE monitoring remains available for the IEC refactor that will wire it in.
    static void onGooseMessage(void* subscriber, void* parameter);

    int numTurbines_;
    bool alarmAcknowledgementEnabled_;
    sc::application::MonitoringDetectors detectors_;
    sc::application::AlarmStateTracker alarmStates_;
    uint64_t lastAlarmResetMs_{0};
};
