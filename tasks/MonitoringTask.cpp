#include <vector>

#include "MonitoringTask.hpp"
#include "common/GlobalDataStructure.hpp"
#include "common/util.hpp"

#define M_PI           3.14159265358979323846  /* pi */


// ---------------------------------------------------------------------------
// Public Interface
// ---------------------------------------------------------------------------

MonitoringTask::MonitoringTask(std::chrono::milliseconds period) : PeriodicTask(period) {
    // TODO: set up GOOSE subscriber via libiec_wrapper
    orientation_state = std::vector<float>(N_TURBINES, 0.0f);
    last_yaw_measurement_time = std::vector<uint64_t>(N_TURBINES, 0);
}

void MonitoringTask::execute() {
    // TODO: implement monitoring loop (thresholds, alarms, GOOSE events)
    std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
    auto& gds = GlobalDataStructure::instance().data();

    if (!gds.systemRunning) return;

    gds.alarmWRecMeas |= checkConsistencyPowerGeneratedVsReceived();
    gds.alarmOrientationMisalign |= checkConsistencyOrientationDynamics();
    gds.alarmWTorqueRotSpd |= checkConsistencyPowerTorqueRotorSpeed();
    gds.alarmHorWdDir |= checkConsistencyWindDirection();
    gds.alarmHorWdDirChg |= checkConsistencyWindDirectionChange();
    gds.alarmHorWdSpdChg |= checkConsistencyWindSpeedChange();

    // TODO: implement reset mechanisms
    uint64_t current_ms = getCurrentTimeMs();
    if (current_ms - last_reset_ms >= 3000) {
        gds.alarmWRecMeas = false;
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

bool MonitoringTask::checkConsistencyOrientationDynamics() {
    // Implementation for checking orientation dynamics consistency
    // TODO: There should be a check here that skips a turbine, if we did not receive a yaw measurement that is recent enough. This is to avoid false alarms due to communication issues.
    auto& gds = GlobalDataStructure::instance().data();

    bool alarm = false;

    for (auto i = 0; i < N_TURBINES; i++) {
        // First predict the next orientation based on the simple model
        float predictedOrientation = (1 - alpha_psi) * orientation_state[i] + alpha_psi * gds.TurbineYawSetpoints[i];

        // Then compare the predicted orientation to the latest received one
        // Only if we did not already use this measurement for detection before (i.e. it is new), we check for an alarm condition
        if (gds.lastYawOffset_t[i] > last_yaw_measurement_time[i] && gds.lastYawOffset_t[i] >= getCurrentTimeMs() - 1000) { // TODO: replace 2000 with yaw_measurement_timeout_ms
            if (abs(predictedOrientation - gds.lastYawOffset[i]) > orientation_threshold) {
                alarm |= true; // Alarm condition met
            }
            last_yaw_measurement_time[i] = gds.lastYawOffset_t[i];
        }
        // if (abs(predictedOrientation - gds.lastYawOffset[i]) > orientation_threshold) {
        //     alarm |= true; // Alarm condition met
        // }

        // Finally update the orientation state using a simple observer-like update (this is a placeholder, more sophisticated estimation could be used)
        orientation_state[i] = predictedOrientation + observer_gain * (gds.lastYawOffset[i] - predictedOrientation);
        gds.orientations[i] = orientation_state[i];
    }   

    return alarm;
}

bool MonitoringTask::checkConsistencyPowerTorqueRotorSpeed() {
    // Implementation for checking power, torque, and rotor speed consistency
    bool alarm = false;
    
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
