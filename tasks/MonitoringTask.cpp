#include <vector>

#include "MonitoringTask.hpp"
#include "common/GlobalDataStructure.hpp"
#include "common/util.hpp"

MonitoringTask::MonitoringTask(std::chrono::milliseconds period) : PeriodicTask(period) {
    orientationState_ = std::vector<float>(kMaxTurbines, 0.0f);
    lastYawMeasurementTimeMs_ = std::vector<uint64_t>(kMaxTurbines, 0);
}

void MonitoringTask::execute() {
    auto& gds = GlobalDataStructure::instance().data();

    if (!gds.systemRunning) return;

    gds.alarmPowerReceivedMismatch |= hasPowerReceivedMismatch();
    gds.alarmOrientationMisalignment |= hasOrientationDynamicsMismatch();
    gds.alarmPowerTorqueRotorSpeedMismatch |= hasPowerTorqueRotorSpeedMismatch();
    gds.alarmWindDirectionMismatch |= hasWindDirectionMismatch();
    gds.alarmWindDirectionChange |= hasWindDirectionStepChange();
    gds.alarmWindSpeedChange |= hasWindSpeedStepChange();

    // The HMI reads alarm latches; reset periodically so transient faults clear
    // without requiring operator input.
    uint64_t currentTimeMs = getCurrentTimeMs();
    if (currentTimeMs - lastResetMs_ >= 3000) {
        gds.alarmPowerReceivedMismatch = false;
        gds.alarmOrientationMisalignment = false;
        gds.alarmPowerTorqueRotorSpeedMismatch = false;
        gds.alarmWindDirectionMismatch = false;
        gds.alarmWindDirectionChange = false;
        gds.alarmWindSpeedChange = false;
        lastResetMs_ = currentTimeMs;
    }
}

void MonitoringTask::onGooseMessage(void* subscriber, void* parameter) {
    (void)subscriber;
    (void)parameter;
}

bool MonitoringTask::hasPowerReceivedMismatch() {
    auto& gds = GlobalDataStructure::instance().data();
    
    return abs(gds.receivedTotalPower - gds.measuredTotalPowerHistory.back()) > 10e6;
}

bool MonitoringTask::hasOutOfBoundsMeasurements() {
    return false;
}

bool MonitoringTask::hasOrientationDynamicsMismatch() {
    auto& gds = GlobalDataStructure::instance().data();
    bool alarm = false;

    for (auto i = 0; i < kMaxTurbines; i++) {
        float predictedOrientation = (1 - orientationAlpha_) * orientationState_[i] + orientationAlpha_ * gds.turbineYawSetpoints[i];

        // Only fresh yaw samples are checked; stale samples usually indicate a
        // communication gap rather than a physical yaw mismatch.
        if (gds.lastYawOffsetTimestampMs[i] > lastYawMeasurementTimeMs_[i] && gds.lastYawOffsetTimestampMs[i] >= getCurrentTimeMs() - kYawMeasurementFreshnessMs) {
            if (abs(predictedOrientation - gds.lastYawOffset[i]) > orientationThresholdDeg_) {
                alarm |= true;
            }
            lastYawMeasurementTimeMs_[i] = gds.lastYawOffsetTimestampMs[i];
        }

        orientationState_[i] = predictedOrientation + orientationObserverGain_ * (gds.lastYawOffset[i] - predictedOrientation);
        gds.orientations[i] = orientationState_[i];
    }   

    return alarm;
}

bool MonitoringTask::hasPowerTorqueRotorSpeedMismatch() {
    auto& gds = GlobalDataStructure::instance().data();

    for (auto i = 0; i < kMaxTurbines; i++) {
        // Compare only samples close enough in time to describe the same state.
        if (abs((int64_t)gds.lastPowerTimestampMs[i] - (int64_t)gds.lastRotorSpeedTimestampMs[i]) > 1000) { // Placeholder threshold of 1 second
            continue;
        }

        float expectedPower = gds.lastRotorSpeed[i] * gds.lastGeneratorTorque[i];
        if (abs(gds.lastPower[i] - expectedPower) > 10e5) { // Placeholder threshold
            return true;
        }
    }

    return false;
}

bool MonitoringTask::hasWindDirectionMismatch() {
    auto& gds = GlobalDataStructure::instance().data();
    for (auto i = 0; i < kMaxTurbines; i++) {
        if (abs(gds.lastWindDirection[i] - gds.farmWindDirection) > 90.0f) { // Placeholder threshold of 90 degrees
            return true;
        }
    }

    return false;
}

bool MonitoringTask::hasWindDirectionStepChange() {
    auto& gds = GlobalDataStructure::instance().data();

    for (auto i = 0; i < kMaxTurbines; i++) {
        if (gds.windDirectionHistory[i].size() >= 2) {
            float windDirectionChange = abs(gds.windDirectionHistory[i].back() - gds.windDirectionHistory[i][gds.windDirectionHistory[i].size() - 2]);
            if (windDirectionChange > 5.0f) { // Placeholder threshold of 5 degrees/s
                return true;
            }
        }
    }

    return false;
}

bool MonitoringTask::hasWindSpeedStepChange() {
    auto& gds = GlobalDataStructure::instance().data();

    for (auto i = 0; i < kMaxTurbines; i++) {
        if (gds.windSpeedHistory[i].size() >= 2) {
            float windSpeedChange = abs(gds.windSpeedHistory[i].back() - gds.windSpeedHistory[i][gds.windSpeedHistory[i].size() - 2]);
            if (windSpeedChange > 1.0f) { // Placeholder threshold of 1 m/s
                return true;
            }
        }
    }

    return false;
}
