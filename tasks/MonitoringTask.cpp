#include <vector>
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <utility>

#include "MonitoringTask.hpp"
#include "common/GlobalDataStructure.hpp"
#include "common/util.hpp"

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

double median(std::vector<double> values)
{
    if (values.empty()) {
        return 0.0;
    }

    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

float circularMeanDeg(const History<double>& values, std::size_t count)
{
    double sinSum = 0.0;
    double cosSum = 0.0;
    const std::size_t n = std::min(count, values.size());

    for (std::size_t i = 0; i < n; ++i) {
        const double radians = values[i] * kPi / 180.0;
        sinSum += std::sin(radians);
        cosSum += std::cos(radians);
    }

    if (sinSum == 0.0 && cosSum == 0.0) {
        return 0.0f;
    }

    return normalizeAngleDeg(static_cast<float>(std::atan2(sinSum, cosSum) * 180.0 / kPi));
}

double yawAdjustedAvailablePower(const GlobalData& gds, int turbineIndex, uint64_t currentMs)
{
    const double windSpeed = gds.lastWS[turbineIndex];
    if (windSpeed < GlobalData::cutInWindSpeed || windSpeed >= GlobalData::cutOutWindSpeed) {
        return 0.0;
    }

    const double rotorRadius = GlobalData::rotorDiameter / 2.0;
    const double sweptArea = kPi * rotorRadius * rotorRadius;
    const double aerodynamicPower =
        0.5 * GlobalData::airDensity * sweptArea *
        GlobalData::optimalPowerCoefficient * windSpeed * windSpeed * windSpeed;

    const bool yawMeasurementRecent =
        gds.lastYawOffset_t[turbineIndex] > 0 &&
        gds.lastYawOffset_t[turbineIndex] + 2000 >= currentMs;
    const double yawAngle = yawMeasurementRecent
        ? gds.lastYawOffset[turbineIndex]
        : static_cast<double>(gds.TurbineYawSetpoints[turbineIndex]);
    const double yawErrorDeg = angularDistanceDeg(
        static_cast<float>(gds.lastWD[turbineIndex]),
        static_cast<float>(yawAngle));
    const double yawCos = std::max(0.0, std::cos(yawErrorDeg * kPi / 180.0));
    const double yawLoss = yawCos * yawCos * yawCos;

    return std::min(aerodynamicPower * yawLoss, GlobalData::ratedPower);
}

double linearRange(const History<double>& values)
{
    if (values.empty()) {
        return 0.0;
    }

    const auto [minIt, maxIt] = std::minmax_element(values.begin(), values.end());
    return *maxIt - *minIt;
}

double angularSpreadDeg(const History<double>& values)
{
    if (values.empty()) {
        return 0.0;
    }

    const float mean = circularMeanDeg(values, values.size());
    double spread = 0.0;
    for (const double value : values) {
        spread = std::max(
            spread,
            static_cast<double>(angularDistanceDeg(mean, static_cast<float>(value))));
    }
    return spread;
}

bool isRecent(uint64_t timestampMs, uint64_t currentMs, uint64_t timeoutMs)
{
    return timestampMs > 0 && timestampMs + timeoutMs >= currentMs;
}

int countDynamicPeers(const TurbineHistory<double>& histories,
                      int turbineIndex,
                      std::size_t minSamples,
                      double dynamicRange,
                      bool angular)
{
    int dynamicPeers = 0;
    for (int peer = 0; peer < static_cast<int>(histories.size()); ++peer) {
        if (peer == turbineIndex || histories[peer].size() < minSamples) {
            continue;
        }

        const double range = angular ? angularSpreadDeg(histories[peer])
                                     : linearRange(histories[peer]);
        if (range > dynamicRange) {
            ++dynamicPeers;
        }
    }
    return dynamicPeers;
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
    wind_speed_change_strike_count = std::vector<int>(N_TURBINES, 0);
    wind_direction_change_strike_count = std::vector<int>(N_TURBINES, 0);
    telemetry_freeze_suspicion_start_time = std::vector<uint64_t>(N_TURBINES, 0);
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
    gds.alarmTelemetryFreezeReplay |= checkConsistencyTelemetryFreezeReplay();

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
        gds.alarmTelemetryFreezeReplay = false;
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

        const bool turbineCommandedOff =
            gds.enableTurbine[i] == 0 ||
            gds.TurbineController[i] == GlobalData::turbineControllerShutdown;

        if (!hasRecentPowerMeasurement ||
            (!turbineCommandedOff &&
             gds.TurbineController[i] != GlobalData::turbineControllerKomega2 &&
             gds.TurbinePowerSetpoints[i] < 0.0f)) {
            power_tracking_mismatch_start_time[i] = 0;
            last_expected_power[i] = -1.0;
            continue;
        }

        const bool hasRecentWindMeasurement =
            gds.lastWS_t[i] > 0 &&
            gds.lastWS_t[i] >= current_ms - power_measurement_timeout_ms;

        double expectedPower = 0.0;
        if (!turbineCommandedOff) {
            if (!hasRecentWindMeasurement) {
                power_tracking_mismatch_start_time[i] = 0;
                last_expected_power[i] = -1.0;
                continue;
            }

            const double yawAdjustedPower = yawAdjustedAvailablePower(gds, static_cast<int>(i), current_ms);
            if (gds.TurbineController[i] == GlobalData::turbineControllerKomega2) {
                expectedPower = yawAdjustedPower;
            } else {
                expectedPower = std::min(
                    static_cast<double>(gds.TurbinePowerSetpoints[i]),
                    yawAdjustedPower);
            }
        }

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


        float expectedPower = gds.generatorEfficiency * gds.lastRPM[i] * 2 * kPi / 60 * gds.lastGenTorque[i] * gds.gearboxRatio; // Placeholder for actual power-torque-speed relation
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
    auto& gds = GlobalDataStructure::instance().data();

    for (auto i = 0; i < N_TURBINES; i++) {
        const auto& history = gds.wdHistory[i];
        if (history.size() >= 5) {
            const float priorMean = circularMeanDeg(history, history.size() - 1);
            const float newest = static_cast<float>(history.back());
            float maxDistanceFromMean = 0.0f;
            const float allMean = circularMeanDeg(history, history.size());
            for (const double value : history) {
                maxDistanceFromMean = std::max(
                    maxDistanceFromMean,
                    angularDistanceDeg(allMean, static_cast<float>(value)));
            }

            const bool suspiciousChange =
                angularDistanceDeg(priorMean, newest) > wind_direction_step_threshold_deg &&
                maxDistanceFromMean > wind_direction_range_threshold_deg;

            if (suspiciousChange) {
                ++wind_direction_change_strike_count[i];
            } else {
                wind_direction_change_strike_count[i] = 0;
            }

            if (wind_direction_change_strike_count[i] >= wind_change_required_strikes) {
                return true;
            }
        } else {
            wind_direction_change_strike_count[i] = 0;
        }
    }

    return false;
}

bool MonitoringTask::checkConsistencyWindSpeedChange() {
    auto& gds = GlobalDataStructure::instance().data();

    for (auto i = 0; i < N_TURBINES; i++) {
        const auto& history = gds.wsHistory[i];
        if (history.size() >= 5) {
            std::vector<double> priorValues;
            priorValues.reserve(history.size() - 1);
            double minValue = history.front();
            double maxValue = history.front();

            for (std::size_t j = 0; j < history.size(); ++j) {
                minValue = std::min(minValue, history[j]);
                maxValue = std::max(maxValue, history[j]);
                if (j + 1 < history.size()) {
                    priorValues.push_back(history[j]);
                }
            }

            const double newest = history.back();
            const double priorMedian = median(std::move(priorValues));
            const bool suspiciousChange =
                std::abs(newest - priorMedian) > wind_speed_step_threshold_ms &&
                (maxValue - minValue) > wind_speed_range_threshold_ms;

            if (suspiciousChange) {
                ++wind_speed_change_strike_count[i];
            } else {
                wind_speed_change_strike_count[i] = 0;
            }

            if (wind_speed_change_strike_count[i] >= wind_change_required_strikes) {
                return true;
            }
        } else {
            wind_speed_change_strike_count[i] = 0;
        }
    }

    return false;
}

bool MonitoringTask::checkConsistencyTelemetryFreezeReplay() {
    auto& gds = GlobalDataStructure::instance().data();
    const uint64_t current_ms = getCurrentTimeMs();
    bool alarm = false;

    const int minDynamicPeers = 2;
    const double peerWsDynamicRange = 0.15;
    const double peerWdDynamicSpread = 1.0;
    const double peerYawDynamicSpread = 1.0;
    const double peerRpmDynamicRange = 0.05;
    const double peerPowerDynamicRangeW = 2.0e4;
    const double peerTorqueDynamicRangeNm = 200.0;

    for (auto i = 0; i < N_TURBINES; i++) {
        const bool turbineCommandedOff =
            gds.enableTurbine[i] == 0 ||
            gds.TurbineController[i] == GlobalData::turbineControllerShutdown;

        if (turbineCommandedOff) {
            telemetry_freeze_suspicion_start_time[i] = 0;
            continue;
        }

        int flatChannels = 0;
        int peerBackedFlatChannels = 0;
        int frozenPlantChannels = 0;
        bool frozenMetChannel = false;

        const auto addEvidence = [&](bool frozen,
                                     bool peerBacked,
                                     bool isMetSignal,
                                     bool isPlantSignal) {
            if (!frozen) {
                return;
            }

            ++flatChannels;
            if (peerBacked) {
                ++peerBackedFlatChannels;
            }
            if (isMetSignal) {
                frozenMetChannel = true;
            }
            if (isPlantSignal) {
                ++frozenPlantChannels;
            }
        };

        const bool wsFrozen =
            gds.wsHistory[i].size() >= telemetry_freeze_window_samples &&
            isRecent(gds.lastWS_t[i], current_ms, telemetry_freeze_measurement_timeout_ms) &&
            linearRange(gds.wsHistory[i]) <= telemetry_freeze_ws_range_ms;
        addEvidence(
            wsFrozen,
            countDynamicPeers(gds.wsHistory, i, telemetry_freeze_window_samples, peerWsDynamicRange, false) >= minDynamicPeers,
            true,
            false);

        const bool wdFrozen =
            gds.wdHistory[i].size() >= telemetry_freeze_window_samples &&
            isRecent(gds.lastWD_t[i], current_ms, telemetry_freeze_measurement_timeout_ms) &&
            angularSpreadDeg(gds.wdHistory[i]) <= telemetry_freeze_wd_range_deg;
        addEvidence(
            wdFrozen,
            countDynamicPeers(gds.wdHistory, i, telemetry_freeze_window_samples, peerWdDynamicSpread, true) >= minDynamicPeers,
            true,
            false);

        const bool yawFrozen =
            gds.yawOffsetHistory[i].size() >= telemetry_freeze_window_samples &&
            isRecent(gds.lastYawOffset_t[i], current_ms, telemetry_freeze_measurement_timeout_ms) &&
            angularSpreadDeg(gds.yawOffsetHistory[i]) <= telemetry_freeze_yaw_range_deg;
        addEvidence(
            yawFrozen,
            countDynamicPeers(gds.yawOffsetHistory, i, telemetry_freeze_window_samples, peerYawDynamicSpread, true) >= minDynamicPeers,
            false,
            false);

        const bool rpmFrozen =
            gds.rpmHistory[i].size() >= telemetry_freeze_window_samples &&
            isRecent(gds.lastRPM_t[i], current_ms, telemetry_freeze_measurement_timeout_ms) &&
            linearRange(gds.rpmHistory[i]) <= telemetry_freeze_rpm_range;
        addEvidence(
            rpmFrozen,
            countDynamicPeers(gds.rpmHistory, i, telemetry_freeze_window_samples, peerRpmDynamicRange, false) >= minDynamicPeers,
            false,
            true);

        const bool powerFrozen =
            gds.powerHistory[i].size() >= telemetry_freeze_window_samples &&
            isRecent(gds.lastPower_t[i], current_ms, telemetry_freeze_measurement_timeout_ms) &&
            linearRange(gds.powerHistory[i]) <= telemetry_freeze_power_range_w;
        addEvidence(
            powerFrozen,
            countDynamicPeers(gds.powerHistory, i, telemetry_freeze_window_samples, peerPowerDynamicRangeW, false) >= minDynamicPeers,
            false,
            true);

        const bool torqueFrozen =
            gds.genTorqueHistory[i].size() >= telemetry_freeze_window_samples &&
            isRecent(gds.lastGenTorque_t[i], current_ms, telemetry_freeze_measurement_timeout_ms) &&
            linearRange(gds.genTorqueHistory[i]) <= telemetry_freeze_torque_range_nm;
        addEvidence(
            torqueFrozen,
            countDynamicPeers(gds.genTorqueHistory, i, telemetry_freeze_window_samples, peerTorqueDynamicRangeNm, false) >= minDynamicPeers,
            false,
            true);

        const bool suspiciousFreeze =
            flatChannels >= 4 &&
            frozenMetChannel &&
            frozenPlantChannels >= 2 &&
            (peerBackedFlatChannels >= 2 || flatChannels >= 5);

        if (!suspiciousFreeze) {
            telemetry_freeze_suspicion_start_time[i] = 0;
            continue;
        }

        if (telemetry_freeze_suspicion_start_time[i] == 0) {
            telemetry_freeze_suspicion_start_time[i] = current_ms;
            continue;
        }

        if (current_ms - telemetry_freeze_suspicion_start_time[i] >= telemetry_freeze_persistence_ms) {
            alarm = true;
        }
    }

    return alarm;
}
