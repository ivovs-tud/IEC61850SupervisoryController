#include <vector>
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <utility>

#include "MonitoringTask.hpp"
#include "common/SharedData.hpp"
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

double averageHistory(const History<double>& values)
{
    if (values.empty()) {
        return 0.0;
    }

    return std::accumulate(values.begin(), values.end(), 0.0) /
           static_cast<double>(values.size());
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

double yawAdjustedAvailablePower(const SharedData& data, int turbineIndex, uint64_t currentMs)
{
    const double windSpeed = isRecent(data.collected.lastWS_t[turbineIndex], currentMs, 2000)
        ? data.collected.lastWS[turbineIndex]
        : static_cast<double>(data.processed.windSpeed);
    const double windDirection = isRecent(data.collected.lastWD_t[turbineIndex], currentMs, 2000)
        ? data.collected.lastWD[turbineIndex]
        : static_cast<double>(data.processed.windDirection);
    if (windSpeed < TurbineParameters::cutInWindSpeed || windSpeed >= TurbineParameters::cutOutWindSpeed) {
        return 0.0;
    }

    const double rotorRadius = TurbineParameters::rotorDiameter / 2.0;
    const double sweptArea = kPi * rotorRadius * rotorRadius;
    const double aerodynamicPower =
        0.5 * TurbineParameters::generatorEfficiency * TurbineParameters::airDensity * sweptArea *
        TurbineParameters::optimalPowerCoefficient * windSpeed * windSpeed * windSpeed;

    const bool yawMeasurementRecent =
        data.collected.lastYawOffset_t[turbineIndex] > 0 &&
        data.collected.lastYawOffset_t[turbineIndex] + 2000 >= currentMs;
    const double yawAngle = yawMeasurementRecent
        ? data.collected.lastYawOffset[turbineIndex]
        : static_cast<double>(data.control.yawSetpoints[turbineIndex]);
    const double yawErrorDeg = angularDistanceDeg(
        static_cast<float>(windDirection),
        static_cast<float>(yawAngle));
    const double yawCos = std::max(0.0, std::cos(yawErrorDeg * kPi / 180.0));
    const double yawLoss = yawCos * yawCos * yawCos;

    return std::min(aerodynamicPower * yawLoss, TurbineParameters::ratedPower);
}

double linearRange(const History<double>& values) {
    if (values.empty()) {
        return 0.0;
    }

    const auto [minIt, maxIt] = std::minmax_element(values.begin(), values.end());
    return *maxIt - *minIt;
}

double angularSpreadDeg(const History<double>& values) {
    if (values.empty()) {
        return 0.0;
    }

    const float mean = circularMeanDeg(values, values.size());
    double spread = 0.0;
    for (const double value : values) {
        spread = std::max(spread, static_cast<double>(angularDistanceDeg(mean, static_cast<float>(value))));
    }

    return spread;
}

bool isTurbineCommandedOff(const SharedData& data, int turbineIndex)
{
    bool enabled = data.control.turbineEnabled[turbineIndex] == 0;
    bool ctrl_shutdown = data.control.turbineController[turbineIndex] == ControlData::controllerShutdown;
    return enabled || ctrl_shutdown;
}

double effectiveYawAdjustedWindSpeed(const SharedData& data, int turbineIndex, uint64_t currentMs)
{
    const double windSpeed = isRecent(data.collected.lastWS_t[turbineIndex], currentMs, 2000) ? data.collected.lastWS[turbineIndex] : static_cast<double>(data.processed.windSpeed);
    const double windDirection = isRecent(data.collected.lastWD_t[turbineIndex], currentMs, 2000) ? data.collected.lastWD[turbineIndex] : static_cast<double>(data.processed.windDirection);
    const bool yawMeasurementRecent = isRecent(data.collected.lastYawOffset_t[turbineIndex], currentMs, 2000);
    const double yawAngle = yawMeasurementRecent ? data.collected.lastYawOffset[turbineIndex] : static_cast<double>(data.control.yawSetpoints[turbineIndex]);
    const double yawErrorDeg = angularDistanceDeg(static_cast<float>(windDirection), static_cast<float>(yawAngle));
    const double yawCos = std::max(0.0, std::cos(yawErrorDeg * kPi / 180.0));
    return std::max(0.0, windSpeed * yawCos);
}

bool expectedPowerForController(const SharedData& data,
                                int turbineIndex,
                                uint64_t currentMs,
                                double& expectedPower,
                                double& yawAdjustedPower)
{
    expectedPower = 0.0;
    yawAdjustedPower = 0.0;

    if (isTurbineCommandedOff(data, turbineIndex)) {
        return true;
    }

    yawAdjustedPower = yawAdjustedAvailablePower(data, turbineIndex, currentMs);
    if (data.control.turbineController[turbineIndex] == ControlData::controllerKomega2) {
        expectedPower = yawAdjustedPower;
        return true;
    }

    if (data.control.powerSetpoints[turbineIndex] < 0.0f) {
        return false;
    }

    expectedPower = std::min(
        static_cast<double>(data.control.powerSetpoints[turbineIndex]),
        yawAdjustedPower);
    return true;
}

double expectedRotorSpeedRpm(const SharedData& data, int turbineIndex, uint64_t currentMs)
{
    const double effectiveWindSpeed = effectiveYawAdjustedWindSpeed(data, turbineIndex, currentMs);
    if (effectiveWindSpeed < TurbineParameters::cutInWindSpeed ||
        effectiveWindSpeed >= TurbineParameters::cutOutWindSpeed) {
        return 0.0;
    }

    const double rotorRadius = TurbineParameters::rotorDiameter / 2.0;
    const double rotorSpeedRadPerSec =
        TurbineParameters::optimalTipSpeedRatio * effectiveWindSpeed / rotorRadius;
    const double rotorSpeedRpm = rotorSpeedRadPerSec * 60.0 / (2.0 * kPi);
    const double minimumRotorSpeedRpm = TurbineParameters::minimumRotorSpeed * 60.0 / (2.0 * kPi);
    return std::clamp(rotorSpeedRpm, minimumRotorSpeedRpm, TurbineParameters::ratedRotorSpeed);
}

double expectedGeneratorTorqueNm(double expectedPower, double expectedRotorSpeedRpm)
{
    const double rotorSpeedRadPerSec = expectedRotorSpeedRpm * 2.0 * kPi / 60.0;
    if (expectedPower <= 0.0 || rotorSpeedRadPerSec <= 0.0) {
        return 0.0;
    }

    const double torque = expectedPower / (TurbineParameters::generatorEfficiency * rotorSpeedRadPerSec * TurbineParameters::gearboxRatio);
    return std::clamp(torque, 0.0, TurbineParameters::maximumGeneratorTorque);
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

MonitoringTask::MonitoringTask(std::chrono::milliseconds period, int numTurbines)
    : PeriodicTask(period), numTurbines_(numTurbines) {
    if (numTurbines_ <= 0) {
        throw std::invalid_argument("MonitoringTask requires at least one turbine");
    }
    // TODO: set up GOOSE subscriber via libiec_wrapper
    orientation_state = std::vector<float>(numTurbines_, 0.0f);
    last_yaw_measurement_time = std::vector<uint64_t>(numTurbines_, 0);
    last_orientation_prediction_time = std::vector<uint64_t>(numTurbines_, 0);
    power_tracking_mismatch_start_time = std::vector<uint64_t>(numTurbines_, 0);
    last_expected_power = std::vector<double>(numTurbines_, -1.0);
    expected_power_history = makeTurbineHistory<double>(numTurbines_, CollectedData::historySize);
    wind_speed_change_strike_count = std::vector<int>(numTurbines_, 0);
    wind_direction_change_strike_count = std::vector<int>(numTurbines_, 0);
    telemetry_freeze_suspicion_start_time = std::vector<std::array<uint64_t, 6>>(numTurbines_);
    for (auto& timers : telemetry_freeze_suspicion_start_time) {
        resetFreezeTimers(timers);
    }
    drivetrain_under_response_start_time = std::vector<uint64_t>(numTurbines_, 0);
    fleet_peer_outlier_start_time = std::vector<uint64_t>(numTurbines_, 0);
}

void MonitoringTask::execute() {
    auto& data = SharedData::instance();
    {
        std::lock_guard<std::mutex> lock(data.interface.mutex);
        if (!data.interface.systemRunning) return;
    }

    bool alarmWRecMeas;
    bool alarmPowerExpected;
    bool alarmOrientationMisalign;
    bool alarmWTorqueRotSpd;
    bool alarmHorWdDir;
    bool alarmHorWdDirChg;
    bool alarmHorWdSpdChg;
    bool alarmTelemetryFreezeReplay;
    bool alarmDrivetrainUnderResponse;
    bool alarmStaticBounds;
    bool alarmFleetPeerOutlier;
    {
        std::scoped_lock lock(data.collected.mutex, data.processed.mutex, data.control.mutex);
        alarmWRecMeas = checkConsistencyPowerGeneratedVsReceived();
        alarmPowerExpected = checkConsistencyMeasuredPowerVsExpected();
        alarmOrientationMisalign = checkConsistencyOrientationDynamics();
        alarmWTorqueRotSpd = checkConsistencyPowerTorqueRotorSpeed();
        alarmHorWdDir = checkConsistencyWindDirection();
        alarmHorWdDirChg = checkConsistencyWindDirectionChange();
        alarmHorWdSpdChg = checkConsistencyWindSpeedChange();
        alarmTelemetryFreezeReplay = checkConsistencyTelemetryFreezeReplay();
        alarmDrivetrainUnderResponse = checkConsistencyDrivetrainUnderResponse();
        alarmStaticBounds = checkStaticTelemetryBounds();
        alarmFleetPeerOutlier = checkConsistencyFleetPeerOutlier();
    }

    std::lock_guard<std::mutex> lock(data.monitoring.mutex);
    auto& alarms = data.monitoring;
    alarms.alarmWRecMeas |= alarmWRecMeas;
    alarms.alarmPowerExpected |= alarmPowerExpected;
    alarms.alarmOrientationMisalign |= alarmOrientationMisalign;
    alarms.alarmWTorqueRotSpd |= alarmWTorqueRotSpd;
    alarms.alarmHorWdDir |= alarmHorWdDir;
    alarms.alarmHorWdDirChg |= alarmHorWdDirChg;
    alarms.alarmHorWdSpdChg |= alarmHorWdSpdChg;
    alarms.alarmTelemetryFreezeReplay |= alarmTelemetryFreezeReplay;
    alarms.alarmDrivetrainUnderResponse |= alarmDrivetrainUnderResponse;
    alarms.alarmStaticBounds |= alarmStaticBounds;
    alarms.alarmFleetPeerOutlier |= alarmFleetPeerOutlier;

    // TODO: implement reset mechanisms
    uint64_t current_ms = getCurrentTimeMs();
    if (current_ms - last_reset_ms >= 3000) {
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
    auto& data = SharedData::instance();
    if (data.processed.measuredTotalPowerHistory.empty()) {
        return false;
    }

    const uint64_t current_ms = getCurrentTimeMs();
    double receivedTotalAverage = 0.0;
    bool hasReceivedPowerHistory = false;
    for (auto i = 0; i < numTurbines_; i++) {
        if (isRecent(data.collected.lastPower_t[i], current_ms, power_measurement_timeout_ms) &&
            !data.collected.powerHistory[i].empty()) {
            receivedTotalAverage += averageHistory(data.collected.powerHistory[i]);
            hasReceivedPowerHistory = true;
        }
    }

    if (!hasReceivedPowerHistory) {
        return false;
    }

    const double measuredTotalAverage = averageHistory(data.processed.measuredTotalPowerHistory);
    return std::abs(receivedTotalAverage - measuredTotalAverage) > 10e6;
}

bool MonitoringTask::checkOutOfBoundsAll() {
    // Implementation for checking out-of-bounds values
    return false;
}

bool MonitoringTask::checkConsistencyMeasuredPowerVsExpected() {
    auto& data = SharedData::instance();
    const uint64_t current_ms = getCurrentTimeMs();

    bool alarm = false;

    for (auto i = 0; i < numTurbines_; i++) {
        const bool hasRecentPowerMeasurement =
            data.collected.lastPower_t[i] > 0 &&
            data.collected.lastPower_t[i] >= current_ms - power_measurement_timeout_ms;

        double expectedPower = 0.0;
        double yawAdjustedPower = 0.0;
        if (!expectedPowerForController(data, static_cast<int>(i), current_ms, expectedPower, yawAdjustedPower)) {
            power_tracking_mismatch_start_time[i] = 0;
            last_expected_power[i] = -1.0;
            expected_power_history[i].clear();
            continue;
        }

        expected_power_history[i].push_back(expectedPower);
        const double expectedPowerAverage = averageHistory(expected_power_history[i]);

        const double tolerance = std::max(
            power_tracking_absolute_tolerance_w,
            power_tracking_relative_tolerance * std::max(expectedPowerAverage, 0.0));

        if (last_expected_power[i] < 0.0 ||
            std::abs(expectedPowerAverage - last_expected_power[i]) > tolerance) {
            power_tracking_mismatch_start_time[i] = 0;
            last_expected_power[i] = expectedPowerAverage;
        }

        const double measuredPowerAverage =
            hasRecentPowerMeasurement && !data.collected.powerHistory[i].empty()
                ? averageHistory(data.collected.powerHistory[i])
                : 0.0;
        const bool closeEnough =
            std::abs(measuredPowerAverage - expectedPowerAverage) <= tolerance;

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
    auto& data = SharedData::instance();
    const uint64_t current_ms = getCurrentTimeMs();

    bool alarm = false;

    for (auto i = 0; i < numTurbines_; i++) {
        const bool hasRecentMeasurement =
            data.collected.lastYawOffset_t[i] > 0 &&
            data.collected.lastYawOffset_t[i] >= current_ms - yaw_measurement_timeout_ms;

        if (last_orientation_prediction_time[i] == 0) {
            if (!hasRecentMeasurement) {
                continue;
            }

            orientation_state[i] = normalizeAngleDeg(static_cast<float>(data.collected.lastYawOffset[i]));
            last_orientation_prediction_time[i] = current_ms;
            last_yaw_measurement_time[i] = data.collected.lastYawOffset_t[i];
            continue;
        }

        const float dt_sec = static_cast<float>(current_ms - last_orientation_prediction_time[i]) / 1000.0f;
        const float predictedOrientation = predictYawAtRate(
            orientation_state[i],
            data.control.yawSetpoints[i],
            static_cast<float>(TurbineParameters::yawingRate),
            dt_sec);

        last_orientation_prediction_time[i] = current_ms;

        // Only check a fresh measurement, and only while it is recent enough to avoid communication-delay false alarms.
        if (hasRecentMeasurement && data.collected.lastYawOffset_t[i] > last_yaw_measurement_time[i]) {
            if (angularDistanceDeg(predictedOrientation, static_cast<float>(data.collected.lastYawOffset[i])) > orientation_threshold) {
                alarm |= true; // Alarm condition met
            }
            last_yaw_measurement_time[i] = data.collected.lastYawOffset_t[i];

            const float measurementError = shortestAngleDiffDeg(
                predictedOrientation,
                static_cast<float>(data.collected.lastYawOffset[i]));
            orientation_state[i] = normalizeAngleDeg(predictedOrientation + observer_gain * measurementError);
        } else {
            orientation_state[i] = predictedOrientation;
        }

    }   

    return alarm;
}

bool MonitoringTask::checkConsistencyPowerTorqueRotorSpeed() {
    // Implementation for checking power, torque, and rotor speed consistency
    auto& data = SharedData::instance();

    for (auto i = 0; i < numTurbines_; i++) {
        // We skip this turbine if the timestamps of the received power torque and rotor speed are too far apart
        if (abs((int64_t)data.collected.lastPower_t[i] - (int64_t)data.collected.lastRPM_t[i]) > 1000) { // Placeholder threshold of 1 second
            continue;
        }
        if(abs((int64_t)data.collected.lastPower_t[i] - (int64_t)data.collected.lastRPM_t[i]) > 1000) { // Placeholder threshold of 1 second
            continue;
        }


        float expectedPower = TurbineParameters::generatorEfficiency * data.collected.lastRPM[i] * 2 * kPi / 60 * data.collected.lastGenTorque[i] * TurbineParameters::gearboxRatio; // Placeholder for actual power-torque-speed relation
        if (abs(data.collected.lastPower[i] - expectedPower) > 4e5) { // Placeholder threshold
            // alarm |= true;
            return true;
        }
    }

    return false;
}

bool MonitoringTask::checkConsistencyWindDirection() {
    // Implementation for checking wind direction consistency
    auto& data = SharedData::instance();
    for (auto i = 0; i < numTurbines_; i++) {
        if (angularDistanceDeg(
                static_cast<float>(data.collected.lastWD[i]),
                static_cast<float>(data.processed.windDirection)) > 25.0f) { // Placeholder threshold of 90 degrees
            return true;
        }
    }

    return false;
}

bool MonitoringTask::checkConsistencyWindDirectionChange() {
    auto& data = SharedData::instance();

    for (auto i = 0; i < numTurbines_; i++) {
        const auto& history = data.collected.wdHistory[i];
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
    auto& data = SharedData::instance();

    for (auto i = 0; i < numTurbines_; i++) {
        const auto& history = data.collected.wsHistory[i];
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
    auto& data = SharedData::instance();
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

    for (auto i = 0; i < numTurbines_; i++) {
        alarm |= updateFreezeTimer(
            static_cast<int>(i),
            FreezeWindSpeed,
            data.collected.wsHistory[i],
            data.collected.lastWS_t[i],
            data.collected.lastWS[i],
            1.0,
            telemetry_freeze_ws_range_ms,
            false);

        alarm |= updateFreezeTimer(
            static_cast<int>(i),
            FreezeWindDirection,
            data.collected.wdHistory[i],
            data.collected.lastWD_t[i],
            data.collected.lastWD[i],
            0.0,
            telemetry_freeze_wd_range_deg,
            true);

        //alar/*m |= updateFreezeTimer(
        //    static_cast<int>(i),
        //    FreezeYaw,
        //    data.collected.yawOffsetHistory[i],
        //    data.collected.lastYawOffset_t[i],
        //    data.collected.lastYawOffset[i],
        //    0.0,
        //    telemetry_freeze_yaw_range_deg,
        //    true);*/

        alarm |= updateFreezeTimer(
            static_cast<int>(i),
            FreezeRpm,
            data.collected.rpmHistory[i],
            data.collected.lastRPM_t[i],
            data.collected.lastRPM[i],
            0.5,
            telemetry_freeze_rpm_range,
            false);

        alarm |= updateFreezeTimer(
            static_cast<int>(i),
            FreezePower,
            data.collected.powerHistory[i],
            data.collected.lastPower_t[i],
            data.collected.lastPower[i],
            1000.0,
            telemetry_freeze_power_range_w,
            false);

        alarm |= updateFreezeTimer(
            static_cast<int>(i),
            FreezeTorque,
            data.collected.genTorqueHistory[i],
            data.collected.lastGenTorque_t[i],
            data.collected.lastGenTorque[i],
            50.0,
            telemetry_freeze_torque_range_nm,
            false);
    }

    return alarm;
}

bool MonitoringTask::checkConsistencyDrivetrainUnderResponse() {
    auto& data = SharedData::instance();
    const uint64_t current_ms = getCurrentTimeMs();
    bool alarm = false;

    for (auto i = 0; i < numTurbines_; i++) {
        if (isTurbineCommandedOff(data, static_cast<int>(i))) {
            drivetrain_under_response_start_time[i] = 0;
            continue;
        }

        const bool hasRecentRpm =
            isRecent(data.collected.lastRPM_t[i], current_ms, power_measurement_timeout_ms);
        const bool hasRecentTorque =
            isRecent(data.collected.lastGenTorque_t[i], current_ms, power_measurement_timeout_ms);

        const double yawAdjustedPower = yawAdjustedAvailablePower(data, static_cast<int>(i), current_ms);
        if (yawAdjustedPower < drivetrain_min_aerodynamic_power_w) {
            drivetrain_under_response_start_time[i] = 0;
            continue;
        }

        double expectedPower = 0.0;
        if (data.control.turbineController[i] == ControlData::controllerKomega2) {
            expectedPower = yawAdjustedPower;
        } else if (data.control.powerSetpoints[i] >= 0.0f) {
            expectedPower = std::min(
                static_cast<double>(data.control.powerSetpoints[i]),
                yawAdjustedPower);
        }

        const double expectedRpm = expectedRotorSpeedRpm(data, static_cast<int>(i), current_ms);
        const double aerodynamicTorque = expectedGeneratorTorqueNm(yawAdjustedPower, expectedRpm);
        const double commandedTorque = expectedGeneratorTorqueNm(expectedPower, expectedRpm);
        if (expectedRpm <= 0.0 || aerodynamicTorque <= 0.0) {
            drivetrain_under_response_start_time[i] = 0;
            continue;
        }

        const bool rpmCollapsed =
            !hasRecentRpm ||
            data.collected.lastRPM[i] < drivetrain_collapsed_rpm_fraction * expectedRpm;
        const bool torqueCollapsed =
            !hasRecentTorque ||
            std::abs(data.collected.lastGenTorque[i]) < std::max(
                drivetrain_collapsed_torque_nm,
                drivetrain_collapsed_torque_fraction * aerodynamicTorque);
        const bool aerodynamicCollapse = rpmCollapsed && torqueCollapsed;

        const bool rpmTooLow =
            !hasRecentRpm ||
            data.collected.lastRPM[i] < drivetrain_rpm_low_fraction * expectedRpm;
        const bool torqueTooLow =
            expectedPower >= drivetrain_min_expected_power_w &&
            commandedTorque > 0.0 &&
            (!hasRecentTorque ||
             data.collected.lastGenTorque[i] < drivetrain_torque_low_fraction * commandedTorque);
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
    auto& data = SharedData::instance();
    const uint64_t current_ms = getCurrentTimeMs();
    const auto freshValueOutOfBounds =
        [&](uint64_t timestampMs, double value, double minValue, double maxValue) {
            return isRecent(timestampMs, current_ms, static_bounds_measurement_timeout_ms) &&
                   (value < minValue || value > maxValue);
        };

    for (auto i = 0; i < numTurbines_; i++) {
        if (freshValueOutOfBounds(
                data.collected.lastWS_t[i],
                data.collected.lastWS[i],
                0.0,
                static_bounds_wind_speed_max_ms)) {
            return true;
        }

        if (freshValueOutOfBounds(data.collected.lastWD_t[i], data.collected.lastWD[i], 0.0, 360.0)) {
            return true;
        }

        if (freshValueOutOfBounds(data.collected.lastYawOffset_t[i], data.collected.lastYawOffset[i], 0.0, 360.0)) {
            return true;
        }

        const bool yawFresh =
            isRecent(data.collected.lastYawOffset_t[i], current_ms, static_bounds_measurement_timeout_ms);
        if (yawFresh &&
            data.processed.windSpeed >= TurbineParameters::cutInWindSpeed &&
            angularDistanceDeg(
                static_cast<float>(data.collected.lastYawOffset[i]),
                static_cast<float>(data.processed.windDirection)) > static_bounds_orientation_window_deg) {
            return true;
        }

        if (freshValueOutOfBounds(data.collected.lastRPM_t[i], data.collected.lastRPM[i], 0.0, static_bounds_rpm_max)) {
            return true;
        }

        if (freshValueOutOfBounds(
                data.collected.lastPower_t[i],
                data.collected.lastPower[i],
                static_bounds_power_min_w,
                static_bounds_power_max_w)) {
            return true;
        }

        if (freshValueOutOfBounds(
                data.collected.lastGenTorque_t[i],
                data.collected.lastGenTorque[i],
                static_bounds_torque_min_nm,
                static_bounds_torque_max_nm)) {
            return true;
        }
    }

    return false;
}

bool MonitoringTask::checkConsistencyFleetPeerOutlier() {
    auto& data = SharedData::instance();
    const uint64_t current_ms = getCurrentTimeMs();
    bool alarm = false;

    if (numTurbines_ < 3) {
        std::fill(fleet_peer_outlier_start_time.begin(), fleet_peer_outlier_start_time.end(), 0);
        return false;
    }

    int operatingTurbines = 0;
    for (auto i = 0; i < numTurbines_; i++) {
        if (!isTurbineCommandedOff(data, static_cast<int>(i))) {
            ++operatingTurbines;
        }
    }

    if (operatingTurbines < numTurbines_ || data.processed.connectedTurbines < numTurbines_) {
        std::fill(fleet_peer_outlier_start_time.begin(), fleet_peer_outlier_start_time.end(), 0);
        return false;
    }

    std::vector<int> turbineIndices;
    std::vector<double> powerRatios;
    std::vector<double> rpmRatios;
    turbineIndices.reserve(static_cast<std::size_t>(numTurbines_));
    powerRatios.reserve(static_cast<std::size_t>(numTurbines_));
    rpmRatios.reserve(static_cast<std::size_t>(numTurbines_));

    for (auto i = 0; i < numTurbines_; i++) {
        if (!isRecent(data.collected.lastPower_t[i], current_ms, power_measurement_timeout_ms) ||
            !isRecent(data.collected.lastRPM_t[i], current_ms, power_measurement_timeout_ms)) {
            continue;
        }

        double expectedPower = 0.0;
        double yawAdjustedPower = 0.0;
        if (!expectedPowerForController(data, static_cast<int>(i), current_ms, expectedPower, yawAdjustedPower) ||
            expectedPower < fleet_peer_min_expected_power_w) {
            continue;
        }

        const double expectedRpm = expectedRotorSpeedRpm(data, static_cast<int>(i), current_ms);
        if (expectedRpm <= 0.0) {
            continue;
        }

        turbineIndices.push_back(static_cast<int>(i));
        powerRatios.push_back(data.collected.lastPower[i] / expectedPower);
        rpmRatios.push_back(data.collected.lastRPM[i] / expectedRpm);
    }

    if (turbineIndices.size() < static_cast<std::size_t>(numTurbines_ - 2)) {
        std::fill(fleet_peer_outlier_start_time.begin(), fleet_peer_outlier_start_time.end(), 0);
        return false;
    }

    const double medianPowerRatio = median(powerRatios);
    const double medianRpmRatio = median(rpmRatios);
    const double powerMad = medianAbsoluteDeviation(powerRatios, medianPowerRatio);
    const double rpmMad = medianAbsoluteDeviation(rpmRatios, medianRpmRatio);
    const double powerThreshold = std::max(fleet_peer_power_ratio_threshold, 4.0 * powerMad);
    const double rpmThreshold = std::max(fleet_peer_rpm_ratio_threshold, 4.0 * rpmMad);

    std::vector<bool> hasMetric(static_cast<std::size_t>(numTurbines_), false);
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

    for (auto i = 0; i < numTurbines_; i++) {
        if (!hasMetric[i]) {
            fleet_peer_outlier_start_time[i] = 0;
        }
    }

    return alarm;
}
