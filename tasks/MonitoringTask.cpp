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

bool isRecent(uint64_t timestampMs, uint64_t currentMs, uint64_t timeoutMs)
{
    return timestampMs > 0 && timestampMs + timeoutMs >= currentMs;
}

double yawAdjustedAvailablePower(const GlobalData& gds, int turbineIndex, uint64_t currentMs)
{
    const double windSpeed = isRecent(gds.lastWS_t[turbineIndex], currentMs, 2000)
        ? gds.lastWS[turbineIndex]
        : static_cast<double>(gds.glob_ws_i);
    const double windDirection = isRecent(gds.lastWD_t[turbineIndex], currentMs, 2000)
        ? gds.lastWD[turbineIndex]
        : static_cast<double>(gds.glob_wd_i);
    if (windSpeed < GlobalData::cutInWindSpeed || windSpeed >= GlobalData::cutOutWindSpeed) {
        return 0.0;
    }

    const double rotorRadius = GlobalData::rotorDiameter / 2.0;
    const double sweptArea = kPi * rotorRadius * rotorRadius;
    const double aerodynamicPower =
        0.5 * GlobalData::generatorEfficiency * GlobalData::airDensity * sweptArea *
        GlobalData::optimalPowerCoefficient * windSpeed * windSpeed * windSpeed;

    const bool yawMeasurementRecent =
        gds.lastYawOffset_t[turbineIndex] > 0 &&
        gds.lastYawOffset_t[turbineIndex] + 2000 >= currentMs;
    const double yawAngle = yawMeasurementRecent
        ? gds.lastYawOffset[turbineIndex]
        : static_cast<double>(gds.TurbineYawSetpoints[turbineIndex]);
    const double yawErrorDeg = angularDistanceDeg(
        static_cast<float>(windDirection),
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

bool isTurbineCommandedOff(const GlobalData& gds, int turbineIndex)
{
    return gds.enableTurbine[turbineIndex] == 0 ||
           gds.TurbineController[turbineIndex] == GlobalData::turbineControllerShutdown;
}

double effectiveYawAdjustedWindSpeed(const GlobalData& gds, int turbineIndex, uint64_t currentMs)
{
    const double windSpeed = isRecent(gds.lastWS_t[turbineIndex], currentMs, 2000)
        ? gds.lastWS[turbineIndex]
        : static_cast<double>(gds.glob_ws_i);
    const double windDirection = isRecent(gds.lastWD_t[turbineIndex], currentMs, 2000)
        ? gds.lastWD[turbineIndex]
        : static_cast<double>(gds.glob_wd_i);
    const bool yawMeasurementRecent =
        isRecent(gds.lastYawOffset_t[turbineIndex], currentMs, 2000);
    const double yawAngle = yawMeasurementRecent
        ? gds.lastYawOffset[turbineIndex]
        : static_cast<double>(gds.TurbineYawSetpoints[turbineIndex]);
    const double yawErrorDeg = angularDistanceDeg(
        static_cast<float>(windDirection),
        static_cast<float>(yawAngle));
    const double yawCos = std::max(0.0, std::cos(yawErrorDeg * kPi / 180.0));
    return std::max(0.0, windSpeed * yawCos);
}

bool expectedPowerForController(const GlobalData& gds,
                                int turbineIndex,
                                uint64_t currentMs,
                                double& expectedPower,
                                double& yawAdjustedPower)
{
    expectedPower = 0.0;
    yawAdjustedPower = 0.0;

    if (isTurbineCommandedOff(gds, turbineIndex)) {
        return true;
    }

    yawAdjustedPower = yawAdjustedAvailablePower(gds, turbineIndex, currentMs);
    if (gds.TurbineController[turbineIndex] == GlobalData::turbineControllerKomega2) {
        expectedPower = yawAdjustedPower;
        return true;
    }

    if (gds.TurbinePowerSetpoints[turbineIndex] < 0.0f) {
        return false;
    }

    expectedPower = std::min(
        static_cast<double>(gds.TurbinePowerSetpoints[turbineIndex]),
        yawAdjustedPower);
    return true;
}

double expectedRotorSpeedRpm(const GlobalData& gds, int turbineIndex, uint64_t currentMs)
{
    const double effectiveWindSpeed = effectiveYawAdjustedWindSpeed(gds, turbineIndex, currentMs);
    if (effectiveWindSpeed < GlobalData::cutInWindSpeed ||
        effectiveWindSpeed >= GlobalData::cutOutWindSpeed) {
        return 0.0;
    }

    const double rotorRadius = GlobalData::rotorDiameter / 2.0;
    const double rotorSpeedRadPerSec =
        GlobalData::optimalTipSpeedRatio * effectiveWindSpeed / rotorRadius;
    const double rotorSpeedRpm = rotorSpeedRadPerSec * 60.0 / (2.0 * kPi);
    const double minimumRotorSpeedRpm = GlobalData::minimumRotorSpeed * 60.0 / (2.0 * kPi);
    return std::clamp(rotorSpeedRpm, minimumRotorSpeedRpm, GlobalData::ratedRotorSpeed);
}

double expectedGeneratorTorqueNm(double expectedPower, double expectedRotorSpeedRpm)
{
    const double rotorSpeedRadPerSec = expectedRotorSpeedRpm * 2.0 * kPi / 60.0;
    if (expectedPower <= 0.0 || rotorSpeedRadPerSec <= 0.0) {
        return 0.0;
    }

    const double torque =
        expectedPower /
        (GlobalData::generatorEfficiency * rotorSpeedRadPerSec * GlobalData::gearboxRatio);
    return std::clamp(torque, 0.0, GlobalData::maximumGeneratorTorque);
}

double medianAbsoluteDeviation(std::vector<double> values, double center)
{
    for (double& value : values) {
        value = std::abs(value - center);
    }
    return median(std::move(values));
}

enum FreezeSignalIndex
{
    FreezeWindSpeed = 0,
    FreezeWindDirection = 1,
    FreezeYaw = 2,
    FreezeRpm = 3,
    FreezePower = 4,
    FreezeTorque = 5,
};

void resetFreezeTimers(std::array<uint64_t, 6>& timers)
{
    timers.fill(0);
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
    telemetry_freeze_suspicion_start_time = std::vector<std::array<uint64_t, 6>>(N_TURBINES);
    for (auto& timers : telemetry_freeze_suspicion_start_time) {
        resetFreezeTimers(timers);
    }
    drivetrain_under_response_start_time = std::vector<uint64_t>(N_TURBINES, 0);
    fleet_peer_outlier_start_time = std::vector<uint64_t>(N_TURBINES, 0);
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
    gds.alarmDrivetrainUnderResponse |= checkConsistencyDrivetrainUnderResponse();
    gds.alarmStaticBounds |= checkStaticTelemetryBounds();
    gds.alarmFleetPeerOutlier |= checkConsistencyFleetPeerOutlier();

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
        gds.alarmDrivetrainUnderResponse = false;
        gds.alarmStaticBounds = false;
        gds.alarmFleetPeerOutlier = false;
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

        double expectedPower = 0.0;
        double yawAdjustedPower = 0.0;
        if (!expectedPowerForController(gds, static_cast<int>(i), current_ms, expectedPower, yawAdjustedPower)) {
            power_tracking_mismatch_start_time[i] = 0;
            last_expected_power[i] = -1.0;
            continue;
        }

        const double tolerance = std::max(
            power_tracking_absolute_tolerance_w,
            power_tracking_relative_tolerance * std::max(expectedPower, 0.0));

        if (last_expected_power[i] < 0.0 || std::abs(expectedPower - last_expected_power[i]) > tolerance) {
            power_tracking_mismatch_start_time[i] = 0;
            last_expected_power[i] = expectedPower;
        }

        const double measuredPower = hasRecentPowerMeasurement ? gds.lastPower[i] : 0.0;
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

    const auto updateFreezeTimer =
        [&](int turbineIndex,
            FreezeSignalIndex signal,
            const History<double>& history,
            uint64_t timestampMs,
            double latestValue,
            double minimumActiveValue,
            double minimumExpectedRange,
            bool angular) {
            auto& signalStartTime =
                telemetry_freeze_suspicion_start_time[turbineIndex][static_cast<std::size_t>(signal)];

            const bool enoughSamples = history.size() >= telemetry_freeze_window_samples;
            const bool freshSignal =
                isRecent(timestampMs, current_ms, telemetry_freeze_measurement_timeout_ms);
            const bool activeSignal = latestValue > minimumActiveValue;
            const double observedRange = angular ? angularSpreadDeg(history) : linearRange(history);
            const bool frozenSignal =
                enoughSamples && freshSignal && activeSignal &&
                observedRange <= minimumExpectedRange;

            if (!frozenSignal) {
                signalStartTime = 0;
                return false;
            }

            if (signalStartTime == 0) {
                signalStartTime = current_ms;
                return false;
            }

            return current_ms - signalStartTime >= telemetry_freeze_persistence_ms;
        };

    for (auto i = 0; i < N_TURBINES; i++) {
        alarm |= updateFreezeTimer(
            static_cast<int>(i),
            FreezeWindSpeed,
            gds.wsHistory[i],
            gds.lastWS_t[i],
            gds.lastWS[i],
            0.0,
            telemetry_freeze_ws_range_ms,
            false);

        alarm |= updateFreezeTimer(
            static_cast<int>(i),
            FreezeWindDirection,
            gds.wdHistory[i],
            gds.lastWD_t[i],
            gds.lastWD[i],
            0.0,
            telemetry_freeze_wd_range_deg,
            true);

        alarm |= updateFreezeTimer(
            static_cast<int>(i),
            FreezeYaw,
            gds.yawOffsetHistory[i],
            gds.lastYawOffset_t[i],
            gds.lastYawOffset[i],
            0.0,
            telemetry_freeze_yaw_range_deg,
            true);

        alarm |= updateFreezeTimer(
            static_cast<int>(i),
            FreezeRpm,
            gds.rpmHistory[i],
            gds.lastRPM_t[i],
            gds.lastRPM[i],
            0.0,
            telemetry_freeze_rpm_range,
            false);

        alarm |= updateFreezeTimer(
            static_cast<int>(i),
            FreezePower,
            gds.powerHistory[i],
            gds.lastPower_t[i],
            gds.lastPower[i],
            0.0,
            telemetry_freeze_power_range_w,
            false);

        alarm |= updateFreezeTimer(
            static_cast<int>(i),
            FreezeTorque,
            gds.genTorqueHistory[i],
            gds.lastGenTorque_t[i],
            gds.lastGenTorque[i],
            0.0,
            telemetry_freeze_torque_range_nm,
            false);
    }

    return alarm;
}

bool MonitoringTask::checkConsistencyDrivetrainUnderResponse() {
    auto& gds = GlobalDataStructure::instance().data();
    const uint64_t current_ms = getCurrentTimeMs();
    bool alarm = false;

    for (auto i = 0; i < N_TURBINES; i++) {
        if (isTurbineCommandedOff(gds, static_cast<int>(i))) {
            drivetrain_under_response_start_time[i] = 0;
            continue;
        }

        const bool hasRecentRpm =
            isRecent(gds.lastRPM_t[i], current_ms, power_measurement_timeout_ms);
        const bool hasRecentTorque =
            isRecent(gds.lastGenTorque_t[i], current_ms, power_measurement_timeout_ms);

        const double yawAdjustedPower = yawAdjustedAvailablePower(gds, static_cast<int>(i), current_ms);
        if (yawAdjustedPower < drivetrain_min_aerodynamic_power_w) {
            drivetrain_under_response_start_time[i] = 0;
            continue;
        }

        double expectedPower = 0.0;
        if (gds.TurbineController[i] == GlobalData::turbineControllerKomega2) {
            expectedPower = yawAdjustedPower;
        } else if (gds.TurbinePowerSetpoints[i] >= 0.0f) {
            expectedPower = std::min(
                static_cast<double>(gds.TurbinePowerSetpoints[i]),
                yawAdjustedPower);
        }

        const double expectedRpm = expectedRotorSpeedRpm(gds, static_cast<int>(i), current_ms);
        const double aerodynamicTorque = expectedGeneratorTorqueNm(yawAdjustedPower, expectedRpm);
        const double commandedTorque = expectedGeneratorTorqueNm(expectedPower, expectedRpm);
        if (expectedRpm <= 0.0 || aerodynamicTorque <= 0.0) {
            drivetrain_under_response_start_time[i] = 0;
            continue;
        }

        const bool rpmCollapsed =
            !hasRecentRpm ||
            gds.lastRPM[i] < drivetrain_collapsed_rpm_fraction * expectedRpm;
        const bool torqueCollapsed =
            !hasRecentTorque ||
            std::abs(gds.lastGenTorque[i]) < std::max(
                drivetrain_collapsed_torque_nm,
                drivetrain_collapsed_torque_fraction * aerodynamicTorque);
        const bool aerodynamicCollapse = rpmCollapsed && torqueCollapsed;

        const bool rpmTooLow =
            !hasRecentRpm ||
            gds.lastRPM[i] < drivetrain_rpm_low_fraction * expectedRpm;
        const bool torqueTooLow =
            expectedPower >= drivetrain_min_expected_power_w &&
            commandedTorque > 0.0 &&
            (!hasRecentTorque ||
             gds.lastGenTorque[i] < drivetrain_torque_low_fraction * commandedTorque);
        const bool underResponse = aerodynamicCollapse || rpmTooLow || torqueTooLow;

        if (!underResponse) {
            drivetrain_under_response_start_time[i] = 0;
            continue;
        }

        if (drivetrain_under_response_start_time[i] == 0) {
            drivetrain_under_response_start_time[i] = current_ms;
            continue;
        }

        const uint64_t requiredGracePeriod = aerodynamicCollapse
            ? drivetrain_aerodynamic_collapse_grace_period_ms
            : drivetrain_under_response_grace_period_ms;
        if (current_ms - drivetrain_under_response_start_time[i] >= requiredGracePeriod) {
            alarm = true;
        }
    }

    return alarm;
}

bool MonitoringTask::checkStaticTelemetryBounds() {
    auto& gds = GlobalDataStructure::instance().data();
    const uint64_t current_ms = getCurrentTimeMs();
    const auto freshValueOutOfBounds =
        [&](uint64_t timestampMs, double value, double minValue, double maxValue) {
            return isRecent(timestampMs, current_ms, static_bounds_measurement_timeout_ms) &&
                   (value < minValue || value > maxValue);
        };

    for (auto i = 0; i < N_TURBINES; i++) {
        if (freshValueOutOfBounds(
                gds.lastWS_t[i],
                gds.lastWS[i],
                0.0,
                static_bounds_wind_speed_max_ms)) {
            return true;
        }

        if (freshValueOutOfBounds(gds.lastWD_t[i], gds.lastWD[i], 0.0, 360.0)) {
            return true;
        }

        if (freshValueOutOfBounds(gds.lastYawOffset_t[i], gds.lastYawOffset[i], 0.0, 360.0)) {
            return true;
        }

        const bool yawFresh =
            isRecent(gds.lastYawOffset_t[i], current_ms, static_bounds_measurement_timeout_ms);
        if (yawFresh &&
            gds.glob_ws_i >= GlobalData::cutInWindSpeed &&
            angularDistanceDeg(
                static_cast<float>(gds.lastYawOffset[i]),
                static_cast<float>(gds.glob_wd_i)) > static_bounds_orientation_window_deg) {
            return true;
        }

        if (freshValueOutOfBounds(gds.lastRPM_t[i], gds.lastRPM[i], 0.0, static_bounds_rpm_max)) {
            return true;
        }

        if (freshValueOutOfBounds(
                gds.lastPower_t[i],
                gds.lastPower[i],
                static_bounds_power_min_w,
                static_bounds_power_max_w)) {
            return true;
        }

        if (freshValueOutOfBounds(
                gds.lastGenTorque_t[i],
                gds.lastGenTorque[i],
                static_bounds_torque_min_nm,
                static_bounds_torque_max_nm)) {
            return true;
        }
    }

    return false;
}

bool MonitoringTask::checkConsistencyFleetPeerOutlier() {
    auto& gds = GlobalDataStructure::instance().data();
    const uint64_t current_ms = getCurrentTimeMs();
    bool alarm = false;

    int operatingTurbines = 0;
    for (auto i = 0; i < N_TURBINES; i++) {
        if (!isTurbineCommandedOff(gds, static_cast<int>(i))) {
            ++operatingTurbines;
        }
    }

    if (operatingTurbines < N_TURBINES || gds.connectedTurbines < N_TURBINES) {
        std::fill(fleet_peer_outlier_start_time.begin(), fleet_peer_outlier_start_time.end(), 0);
        return false;
    }

    std::vector<int> turbineIndices;
    std::vector<double> powerRatios;
    std::vector<double> rpmRatios;
    turbineIndices.reserve(N_TURBINES);
    powerRatios.reserve(N_TURBINES);
    rpmRatios.reserve(N_TURBINES);

    for (auto i = 0; i < N_TURBINES; i++) {
        if (!isRecent(gds.lastPower_t[i], current_ms, power_measurement_timeout_ms) ||
            !isRecent(gds.lastRPM_t[i], current_ms, power_measurement_timeout_ms)) {
            continue;
        }

        double expectedPower = 0.0;
        double yawAdjustedPower = 0.0;
        if (!expectedPowerForController(gds, static_cast<int>(i), current_ms, expectedPower, yawAdjustedPower) ||
            expectedPower < fleet_peer_min_expected_power_w) {
            continue;
        }

        const double expectedRpm = expectedRotorSpeedRpm(gds, static_cast<int>(i), current_ms);
        if (expectedRpm <= 0.0) {
            continue;
        }

        turbineIndices.push_back(static_cast<int>(i));
        powerRatios.push_back(gds.lastPower[i] / expectedPower);
        rpmRatios.push_back(gds.lastRPM[i] / expectedRpm);
    }

    if (turbineIndices.size() < static_cast<std::size_t>(N_TURBINES - 2)) {
        std::fill(fleet_peer_outlier_start_time.begin(), fleet_peer_outlier_start_time.end(), 0);
        return false;
    }

    const double medianPowerRatio = median(powerRatios);
    const double medianRpmRatio = median(rpmRatios);
    const double powerMad = medianAbsoluteDeviation(powerRatios, medianPowerRatio);
    const double rpmMad = medianAbsoluteDeviation(rpmRatios, medianRpmRatio);
    const double powerThreshold = std::max(fleet_peer_power_ratio_threshold, 4.0 * powerMad);
    const double rpmThreshold = std::max(fleet_peer_rpm_ratio_threshold, 4.0 * rpmMad);

    std::vector<bool> hasMetric(N_TURBINES, false);
    for (std::size_t k = 0; k < turbineIndices.size(); ++k) {
        const int turbineIndex = turbineIndices[k];
        hasMetric[turbineIndex] = true;

        const bool powerOutlier =
            std::abs(powerRatios[k] - medianPowerRatio) > powerThreshold;
        const bool rpmOutlier =
            std::abs(rpmRatios[k] - medianRpmRatio) > rpmThreshold;
        const bool suspiciousOutlier = powerOutlier || rpmOutlier;

        if (!suspiciousOutlier) {
            fleet_peer_outlier_start_time[turbineIndex] = 0;
            continue;
        }

        if (fleet_peer_outlier_start_time[turbineIndex] == 0) {
            fleet_peer_outlier_start_time[turbineIndex] = current_ms;
            continue;
        }

        if (current_ms - fleet_peer_outlier_start_time[turbineIndex] >= fleet_peer_outlier_grace_period_ms) {
            alarm = true;
        }
    }

    for (auto i = 0; i < N_TURBINES; i++) {
        if (!hasMetric[i]) {
            fleet_peer_outlier_start_time[i] = 0;
        }
    }

    return alarm;
}
