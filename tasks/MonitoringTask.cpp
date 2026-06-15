#include <vector>
#include <algorithm>
#include <cmath>

#include "MonitoringTask.hpp"
#include "common/GlobalDataStructure.hpp"
#include "common/util.hpp"

#define M_PI           3.14159265358979323846  /* pi */

namespace {
float normalizeAngleDeg(float angle)
{
    angle = std::fmod(angle, 360.0f);
    if (angle < 0.0f) {
        angle += 360.0f;
    }
    return angle;
}

float shortestAngleDiffDeg(float from, float to)
{
    float diff = normalizeAngleDeg(to) - normalizeAngleDeg(from);
    if (diff > 180.0f) {
        diff -= 360.0f;
    } else if (diff < -180.0f) {
        diff += 360.0f;
    }
    return diff;
}

float angularDistanceDeg(float a, float b)
{
    return std::abs(shortestAngleDiffDeg(a, b));
}

float predictYawAtRate(float currentOrientation, float yawSetpoint, float yawingRateDegPerSec, float dtSec)
{
    const float diff = shortestAngleDiffDeg(currentOrientation, yawSetpoint);
    const float maxStep = std::max(0.0f, yawingRateDegPerSec * dtSec);
    const float step = std::clamp(diff, -maxStep, maxStep);
    return normalizeAngleDeg(currentOrientation + step);
}
}


// ---------------------------------------------------------------------------
// Public Interface
// ---------------------------------------------------------------------------

MonitoringTask::MonitoringTask(std::chrono::milliseconds period) : PeriodicTask(period) {
    // TODO: set up GOOSE subscriber via libiec_wrapper
    orientation_state = std::vector<float>(N_TURBINES, 0.0f);
    last_yaw_measurement_time = std::vector<uint64_t>(N_TURBINES, 0);
    last_orientation_prediction_time = std::vector<uint64_t>(N_TURBINES, 0);
    power_tracking_mismatch_start_time = std::vector<uint64_t>(N_TURBINES, 0);
    last_expected_power = std::vector<double>(N_TURBINES, -1.0);
}

void MonitoringTask::execute() {
    // TODO: implement monitoring loop (thresholds, alarms, GOOSE events)
    std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
    auto& gds = GlobalDataStructure::instance().data();

    if (!gds.systemRunning) return;

    gds.alarmWRecMeas |= checkConsistencyPowerGeneratedVsReceived();
    gds.alarmPowerExpected |= checkConsistencyMeasuredPowerVsExpected();
    gds.alarmOrientationMisalign |= checkConsistencyOrientationDynamics();
    gds.alarmWTorqueRotSpd |= checkConsistencyPowerTorqueRotorSpeed();
    gds.alarmHorWdDir |= checkConsistencyWindDirection();
    gds.alarmHorWdDirChg |= checkConsistencyWindDirectionChange();
    gds.alarmHorWdSpdChg |= checkConsistencyWindSpeedChange();

    // TODO: implement reset mechanisms
    uint64_t current_ms = getCurrentTimeMs();
    if (current_ms - last_reset_ms >= 3000) {
        gds.alarmWRecMeas = false;
        gds.alarmPowerExpected = false;
        gds.alarmOrientationMisalign = false;
        gds.alarmWTorqueRotSpd = false;
        gds.alarmHorWdDir = false;
        gds.alarmHorWdDirChg = false;
        gds.alarmHorWdSpdChg = false;
        last_reset_ms = current_ms;
    }
}





void MonitoringTask::onGooseMessage(void* subscriber, void* parameter) {
    // TODO: handle incoming GOOSE message
    (void)subscriber;
    (void)parameter;
}


// ---------------------------------------------------------------------------
// Private monitoring functions
// ---------------------------------------------------------------------------

bool MonitoringTask::checkConsistencyPowerGeneratedVsReceived() {
    //float total = 0;
    //std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
    auto& gds = GlobalDataStructure::instance().data();
    if (gds.Wtotal_meas.empty()) {
        return false;
    }
    
    /*for (auto i = 0; i < gds.powerHistory.size(); i++) {
		total += gds.powerHistory[i].back();
    }*/
    
    return abs(gds.TotalPower_recv - gds.Wtotal_meas.back()) > 10e6;
}

bool MonitoringTask::checkOutOfBoundsAll() {
    // Implementation for checking out-of-bounds values
    return false;
}

bool MonitoringTask::checkConsistencyMeasuredPowerVsExpected() {
    auto& gds = GlobalDataStructure::instance().data();
    const uint64_t current_ms = getCurrentTimeMs();

    bool alarm = false;

    for (auto i = 0; i < N_TURBINES; i++) {
        const bool hasRecentPowerMeasurement =
            gds.lastPower_t[i] > 0 &&
            gds.lastPower_t[i] >= current_ms - power_measurement_timeout_ms;

        if (!hasRecentPowerMeasurement || gds.TurbinePowerSetpoints[i] < 0.0f) {
            power_tracking_mismatch_start_time[i] = 0;
            last_expected_power[i] = -1.0;
            continue;
        }

        const double expectedPower = std::min(
            static_cast<double>(gds.TurbinePowerSetpoints[i]),
            std::max(0.0, gds.AvailablePower[i]));

        const double tolerance = std::max(
            power_tracking_absolute_tolerance_w,
            power_tracking_relative_tolerance * std::max(expectedPower, 0.0));

        if (last_expected_power[i] < 0.0 || std::abs(expectedPower - last_expected_power[i]) > tolerance) {
            power_tracking_mismatch_start_time[i] = 0;
            last_expected_power[i] = expectedPower;
        }

        const double measuredPower = gds.lastPower[i];
        const bool closeEnough = std::abs(measuredPower - expectedPower) <= tolerance;

        if (closeEnough) {
            power_tracking_mismatch_start_time[i] = 0;
            continue;
        }

        if (power_tracking_mismatch_start_time[i] == 0) {
            power_tracking_mismatch_start_time[i] = current_ms;
            continue;
        }

        if (current_ms - power_tracking_mismatch_start_time[i] >= power_tracking_grace_period_ms) {
            alarm = true;
        }
    }

    return alarm;
}

bool MonitoringTask::checkConsistencyOrientationDynamics() {
    auto& gds = GlobalDataStructure::instance().data();
    const uint64_t current_ms = getCurrentTimeMs();

    bool alarm = false;

    for (auto i = 0; i < N_TURBINES; i++) {
        const bool hasRecentMeasurement =
            gds.lastYawOffset_t[i] > 0 &&
            gds.lastYawOffset_t[i] >= current_ms - yaw_measurement_timeout_ms;

        if (last_orientation_prediction_time[i] == 0) {
            if (!hasRecentMeasurement) {
                continue;
            }

            orientation_state[i] = normalizeAngleDeg(static_cast<float>(gds.lastYawOffset[i]));
            gds.orientations[i] = orientation_state[i];
            last_orientation_prediction_time[i] = current_ms;
            last_yaw_measurement_time[i] = gds.lastYawOffset_t[i];
            continue;
        }

        const float dt_sec = static_cast<float>(current_ms - last_orientation_prediction_time[i]) / 1000.0f;
        const float predictedOrientation = predictYawAtRate(
            orientation_state[i],
            gds.TurbineYawSetpoints[i],
            static_cast<float>(GlobalData::yawingRate),
            dt_sec);

        last_orientation_prediction_time[i] = current_ms;

        // Only check a fresh measurement, and only while it is recent enough to avoid communication-delay false alarms.
        if (hasRecentMeasurement && gds.lastYawOffset_t[i] > last_yaw_measurement_time[i]) {
            if (angularDistanceDeg(predictedOrientation, static_cast<float>(gds.lastYawOffset[i])) > orientation_threshold) {
                alarm |= true; // Alarm condition met
            }
            last_yaw_measurement_time[i] = gds.lastYawOffset_t[i];

            const float measurementError = shortestAngleDiffDeg(
                predictedOrientation,
                static_cast<float>(gds.lastYawOffset[i]));
            orientation_state[i] = normalizeAngleDeg(predictedOrientation + observer_gain * measurementError);
        } else {
            orientation_state[i] = predictedOrientation;
        }

        gds.orientations[i] = orientation_state[i];
    }   

    return alarm;
}

bool MonitoringTask::checkConsistencyPowerTorqueRotorSpeed() {
    // Implementation for checking power, torque, and rotor speed consistency
    auto& gds = GlobalDataStructure::instance().data();

    for (auto i = 0; i < N_TURBINES; i++) {
        // We skip this turbine if the timestamps of the received power torque and rotor speed are too far apart
        if (abs((int64_t)gds.lastPower_t[i] - (int64_t)gds.lastRPM_t[i]) > 1000) { // Placeholder threshold of 1 second
            continue;
        }
        if(abs((int64_t)gds.lastPower_t[i] - (int64_t)gds.lastRPM_t[i]) > 1000) { // Placeholder threshold of 1 second
            continue;
        }


        float expectedPower = gds.generatorEfficiency * gds.lastRPM[i] * 2 * M_PI / 60 * gds.lastGenTorque[i] * gds.gearboxRatio; // Placeholder for actual power-torque-speed relation
        if (abs(gds.lastPower[i] - expectedPower) > 4e5) { // Placeholder threshold
            // alarm |= true;
            return true;
        }
    }

    return false;
}

bool MonitoringTask::checkConsistencyWindDirection() {
    // Implementation for checking wind direction consistency
    auto& gds = GlobalDataStructure::instance().data();
    for (auto i = 0; i < N_TURBINES; i++) {
        if (abs(gds.lastWD[i] - gds.glob_wd_i) > 90.0f) { // Placeholder threshold of 90 degrees
            return true;
        }
    }

    return false;
}

bool MonitoringTask::checkConsistencyWindDirectionChange() {
    // Implementation for checking wind direction change consistency
    auto& gds = GlobalDataStructure::instance().data();

    for (auto i = 0; i < N_TURBINES; i++) {
        // Take the two most recent values of the wind direction history and compare
        if (gds.wdHistory[i].size() >= 2) {
            float wd_change = abs(gds.wdHistory[i].back() - gds.wdHistory[i][gds.wdHistory[i].size() - 2]);
            if (wd_change > 5.0f) { // Placeholder threshold of 5 degrees/s
                return true;
            }
        }
    }

    return false;
}

bool MonitoringTask::checkConsistencyWindSpeedChange() {
    // Implementation for checking wind speed change consistency

        auto& gds = GlobalDataStructure::instance().data();

    for (auto i = 0; i < N_TURBINES; i++) {
        // Take the two most recent values of the wind speed history and compare
        if (gds.wsHistory[i].size() >= 2) {
            float ws_change = abs(gds.wsHistory[i].back() - gds.wsHistory[i][gds.wsHistory[i].size() - 2]);
            if (ws_change > 1.0f) { // Placeholder threshold of 1 m/s
                return true;
            }
        }
    }

    return false;
}
