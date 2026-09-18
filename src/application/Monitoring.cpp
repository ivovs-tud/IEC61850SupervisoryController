#include "sc/application/Monitoring.hpp"

#include "sc/TurbineParameters.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace sc::application {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr std::size_t expectedPowerHistoryCapacity = 20;
constexpr uint64_t powerMeasurementTimeoutMs = 2000;
constexpr uint64_t powerTrackingGracePeriodMs = 5000;
constexpr double powerTrackingAbsoluteToleranceW = 2e6;
constexpr double powerTrackingRelativeTolerance = 0.1;
constexpr float orientationThresholdDeg = 8.0F;
constexpr float orientationObserverGain = 0.5F;
constexpr uint64_t yawMeasurementTimeoutMs = 1000;
constexpr double windSpeedStepThresholdMs = 3.0;
constexpr double windSpeedRangeThresholdMs = 4.0;
constexpr double windDirectionStepThresholdDeg = 25.0;
constexpr double windDirectionRangeThresholdDeg = 40.0;
constexpr int windChangeRequiredStrikes = 3;
constexpr std::size_t freezeWindowSamples = 8;
constexpr uint64_t freezePersistenceMs = 1500;
constexpr uint64_t freezeMeasurementTimeoutMs = 3000;
constexpr uint64_t drivetrainUnderResponseGracePeriodMs = 5000;
constexpr uint64_t drivetrainCollapseGracePeriodMs = 1500;
constexpr double drivetrainMinimumExpectedPowerW = 5.0e5;
constexpr double drivetrainMinimumAerodynamicPowerW = 2.0e5;
constexpr double drivetrainRpmLowFraction = 0.70;
constexpr double drivetrainTorqueLowFraction = 0.60;
constexpr double drivetrainCollapsedRpmFraction = 0.15;
constexpr double drivetrainCollapsedTorqueFraction = 0.15;
constexpr double drivetrainCollapsedTorqueNm = 200.0;
constexpr uint64_t staticBoundsMeasurementTimeoutMs = 3000;
constexpr double staticBoundsWindSpeedMaxMs = 1.5 * sc::TurbineParameters::cutOutWindSpeed;
constexpr double staticBoundsOrientationWindowDeg = 60.0;
constexpr double staticBoundsRpmMax = 1.5 * sc::TurbineParameters::ratedRotorSpeed;
constexpr double staticBoundsPowerMinW = -0.10 * sc::TurbineParameters::ratedPower;
constexpr double staticBoundsPowerMaxW = 1.25 * sc::TurbineParameters::ratedPower;
constexpr double staticBoundsTorqueMinNm = -0.10 * sc::TurbineParameters::maximumGeneratorTorque;
constexpr double staticBoundsTorqueMaxNm = 1.50 * sc::TurbineParameters::maximumGeneratorTorque;
constexpr uint64_t fleetPeerOutlierGracePeriodMs = 8000;
constexpr double fleetPeerMinimumExpectedPowerW = 1.0e6;
constexpr double fleetPeerPowerRatioThreshold = 0.30;
constexpr double fleetPeerRpmRatioThreshold = 0.25;

enum FreezeSignal : std::size_t {
    FreezeWindSpeed,
    FreezeWindDirection,
    FreezeRotorSpeed,
    FreezePower,
    FreezeTorque
};

bool isRecent(uint64_t timestampMs, uint64_t currentTimeMs, uint64_t timeoutMs) {
    return timestampMs != 0 && (timestampMs >= currentTimeMs || currentTimeMs - timestampMs <= timeoutMs);
}

double average(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

double medianAbsoluteDeviation(std::vector<double> values, double center) {
    for (double& value : values) value = std::abs(value - center);
    return median(std::move(values));
}

float normalizeAngleDeg(float angle) {
    angle = std::fmod(angle, 360.0F);
    if (angle < 0.0F) angle += 360.0F;
    return angle;
}

float shortestAngleDiffDeg(float from, float to) {
    float difference = normalizeAngleDeg(to) - normalizeAngleDeg(from);
    if (difference > 180.0F) difference -= 360.0F;
    if (difference < -180.0F) difference += 360.0F;
    return difference;
}

float angularDistanceDeg(float first, float second) {
    return std::abs(shortestAngleDiffDeg(first, second));
}

float circularMeanDeg(const std::vector<double>& values, std::size_t count) {
    double sinSum = 0.0;
    double cosSum = 0.0;
    const std::size_t sampleCount = std::min(count, values.size());
    for (std::size_t index = 0; index < sampleCount; ++index) {
        const double radians = values[index] * pi / 180.0;
        sinSum += std::sin(radians);
        cosSum += std::cos(radians);
    }
    if (sinSum == 0.0 && cosSum == 0.0) return 0.0F;
    return normalizeAngleDeg(static_cast<float>(std::atan2(sinSum, cosSum) * 180.0 / pi));
}

double linearRange(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    const auto [minimum, maximum] = std::minmax_element(values.begin(), values.end());
    return *maximum - *minimum;
}

double angularSpreadDeg(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    const float mean = circularMeanDeg(values, values.size());
    double spread = 0.0;
    for (double value : values) {
        spread = std::max(spread, static_cast<double>(angularDistanceDeg(mean, static_cast<float>(value))));
    }
    return spread;
}

float predictYawAtRate(float current, float setpoint, float rateDegPerSecond, float elapsedSeconds) {
    const float difference = shortestAngleDiffDeg(current, setpoint);
    const float maximumStep = std::max(0.0F, rateDegPerSecond * elapsedSeconds);
    return normalizeAngleDeg(current + std::clamp(difference, -maximumStep, maximumStep));
}

void addEvidence(MonitoringResult& result, AlarmType alarm, int turbineId, double measured,
                 double expected, double threshold, uint64_t timestampMs) {
    result.active[static_cast<std::size_t>(alarm)] = true;
    result.evidence.push_back({alarm, turbineId, measured, expected, threshold, timestampMs});
}

void appendHistory(std::vector<double>& history, double value) {
    if (history.size() == expectedPowerHistoryCapacity) history.erase(history.begin());
    history.push_back(value);
}

double yawAdjustedAvailablePower(const TurbineMonitoringInput& turbine, const MonitoringInput& input) {
    const double windSpeed = isRecent(turbine.windSpeedTimeMs, input.currentTimeMs, powerMeasurementTimeoutMs)
        ? turbine.windSpeed : static_cast<double>(input.farmWindSpeed);
    const double windDirection = isRecent(turbine.windDirectionTimeMs, input.currentTimeMs, powerMeasurementTimeoutMs)
        ? turbine.windDirection : static_cast<double>(input.farmWindDirection);
    if (windSpeed < sc::TurbineParameters::cutInWindSpeed || windSpeed >= sc::TurbineParameters::cutOutWindSpeed) {
        return 0.0;
    }

    const double rotorRadius = sc::TurbineParameters::rotorDiameter / 2.0;
    const double sweptArea = pi * rotorRadius * rotorRadius;
    const double aerodynamicPower = 0.5 * sc::TurbineParameters::generatorEfficiency *
        sc::TurbineParameters::airDensity * sweptArea * sc::TurbineParameters::optimalPowerCoefficient *
        windSpeed * windSpeed * windSpeed;
    const double yaw = isRecent(turbine.yawTimeMs, input.currentTimeMs, powerMeasurementTimeoutMs)
        ? turbine.yaw : static_cast<double>(turbine.yawSetpoint);
    const double yawErrorDeg = angularDistanceDeg(static_cast<float>(windDirection), static_cast<float>(yaw));
    const double yawCosine = std::max(0.0, std::cos(yawErrorDeg * pi / 180.0));
    return std::min(aerodynamicPower * yawCosine * yawCosine * yawCosine, sc::TurbineParameters::ratedPower);
}

double effectiveYawAdjustedWindSpeed(const TurbineMonitoringInput& turbine, const MonitoringInput& input) {
    const double windSpeed = isRecent(turbine.windSpeedTimeMs, input.currentTimeMs, powerMeasurementTimeoutMs)
        ? turbine.windSpeed : static_cast<double>(input.farmWindSpeed);
    const double windDirection = isRecent(turbine.windDirectionTimeMs, input.currentTimeMs, powerMeasurementTimeoutMs)
        ? turbine.windDirection : static_cast<double>(input.farmWindDirection);
    const double yaw = isRecent(turbine.yawTimeMs, input.currentTimeMs, powerMeasurementTimeoutMs)
        ? turbine.yaw : static_cast<double>(turbine.yawSetpoint);
    const double yawErrorDeg = angularDistanceDeg(static_cast<float>(windDirection), static_cast<float>(yaw));
    return std::max(0.0, windSpeed * std::max(0.0, std::cos(yawErrorDeg * pi / 180.0)));
}

double expectedRotorSpeedRpm(const TurbineMonitoringInput& turbine, const MonitoringInput& input) {
    const double windSpeed = effectiveYawAdjustedWindSpeed(turbine, input);
    if (windSpeed < sc::TurbineParameters::cutInWindSpeed || windSpeed >= sc::TurbineParameters::cutOutWindSpeed) {
        return 0.0;
    }
    const double rotorRadius = sc::TurbineParameters::rotorDiameter / 2.0;
    const double rotorSpeedRadPerSecond = sc::TurbineParameters::optimalTipSpeedRatio * windSpeed / rotorRadius;
    const double rotorSpeedRpm = rotorSpeedRadPerSecond * 60.0 / (2.0 * pi);
    const double minimumRotorSpeedRpm = sc::TurbineParameters::minimumRotorSpeed * 60.0 / (2.0 * pi);
    return std::clamp(rotorSpeedRpm, minimumRotorSpeedRpm, sc::TurbineParameters::ratedRotorSpeed);
}

double expectedGeneratorTorqueNm(double power, double rotorSpeedRpm) {
    const double rotorSpeedRadPerSecond = rotorSpeedRpm * 2.0 * pi / 60.0;
    if (power <= 0.0 || rotorSpeedRadPerSecond <= 0.0) return 0.0;
    const double torque = power / (sc::TurbineParameters::generatorEfficiency * rotorSpeedRadPerSecond *
                                   sc::TurbineParameters::gearboxRatio);
    return std::clamp(torque, 0.0, sc::TurbineParameters::maximumGeneratorTorque);
}

bool expectedPowerForController(const TurbineMonitoringInput& turbine, const MonitoringInput& input,
                                double& expectedPower, double& availablePower) {
    expectedPower = 0.0;
    availablePower = 0.0;
    if (turbine.commandedOff) return true;
    availablePower = yawAdjustedAvailablePower(turbine, input);
    if (turbine.maximumPowerMode) {
        expectedPower = availablePower;
        return true;
    }
    if (turbine.powerSetpoint < 0.0F) return false;
    expectedPower = std::min(static_cast<double>(turbine.powerSetpoint), availablePower);
    return true;
}

} // namespace

bool MonitoringResult::isActive(AlarmType alarm) const {
    return active[static_cast<std::size_t>(alarm)];
}

void AlarmStateTracker::update(const MonitoringResult& result) {
    for (std::size_t index = 0; index < states_.size(); ++index) {
        auto& alarm = states_[index];
        const bool active = result.active[index];
        if (active) {
            if (!alarm.latched) alarm.acknowledged = false;
            alarm.active = true;
            alarm.latched = true;
            continue;
        }

        alarm.active = false;
        if (alarm.acknowledged) {
            alarm.latched = false;
            alarm.acknowledged = false;
        }
    }
}

void AlarmStateTracker::acknowledge() {
    for (auto& alarm : states_) {
        if (!alarm.latched) continue;
        if (alarm.active) {
            alarm.acknowledged = true;
        } else {
            alarm = {};
        }
    }
}

void AlarmStateTracker::reset() {
    states_.fill({});
}

const AlarmState& AlarmStateTracker::state(AlarmType alarm) const {
    if (alarm == AlarmType::Count) throw std::invalid_argument("AlarmType::Count has no alarm state");
    return states_[static_cast<std::size_t>(alarm)];
}

bool AlarmStateTracker::visible(AlarmType alarm) const {
    const auto& alarmState = state(alarm);
    return alarmState.active || alarmState.latched;
}

MonitoringDetectors::MonitoringDetectors(std::size_t turbineCount) : turbineCount_(turbineCount) {
    if (turbineCount_ == 0) throw std::invalid_argument("MonitoringDetectors requires at least one turbine");
    reset();
}

void MonitoringDetectors::reset() {
    lastYawMeasurementTime_.assign(turbineCount_, 0);
    lastOrientationPredictionTime_.assign(turbineCount_, 0);
    orientation_.assign(turbineCount_, 0.0F);
    powerMismatchStartTime_.assign(turbineCount_, 0);
    lastExpectedPower_.assign(turbineCount_, -1.0);
    expectedPowerHistory_.assign(turbineCount_, {});
    windSpeedChangeStrikes_.assign(turbineCount_, 0);
    windDirectionChangeStrikes_.assign(turbineCount_, 0);
    freezeStartTime_.assign(turbineCount_, {});
    drivetrainUnderResponseStartTime_.assign(turbineCount_, 0);
    fleetPeerOutlierStartTime_.assign(turbineCount_, 0);
}

MonitoringResult MonitoringDetectors::evaluate(const MonitoringInput& input) {
    if (input.turbines.size() != turbineCount_) {
        throw std::invalid_argument("monitoring input does not match configured turbine count");
    }
    MonitoringResult result;
    checkPowerBalance(input, result);
    checkPowerTracking(input, result);
    checkOrientation(input, result);
    checkPowerTorqueRotorSpeed(input, result);
    checkWindDirection(input, result);
    checkWindDirectionChange(input, result);
    checkWindSpeedChange(input, result);
    checkTelemetryFreeze(input, result);
    checkDrivetrainResponse(input, result);
    checkStaticBounds(input, result);
    checkFleetPeerOutliers(input, result);
    return result;
}

MonitoringResult MonitoringDetectors::evaluateDetector(AlarmType alarm, const MonitoringInput& input) {
    if (input.turbines.size() != turbineCount_) {
        throw std::invalid_argument("monitoring input does not match configured turbine count");
    }
    MonitoringResult result;
    switch (alarm) {
    case AlarmType::PowerGeneratedVsReceived: checkPowerBalance(input, result); break;
    case AlarmType::MeasuredPowerVsExpected: checkPowerTracking(input, result); break;
    case AlarmType::OrientationMisalignment: checkOrientation(input, result); break;
    case AlarmType::PowerTorqueRotorSpeed: checkPowerTorqueRotorSpeed(input, result); break;
    case AlarmType::WindDirection: checkWindDirection(input, result); break;
    case AlarmType::WindDirectionChange: checkWindDirectionChange(input, result); break;
    case AlarmType::WindSpeedChange: checkWindSpeedChange(input, result); break;
    case AlarmType::TelemetryFreeze: checkTelemetryFreeze(input, result); break;
    case AlarmType::DrivetrainUnderResponse: checkDrivetrainResponse(input, result); break;
    case AlarmType::StaticTelemetryBounds: checkStaticBounds(input, result); break;
    case AlarmType::FleetPeerOutlier: checkFleetPeerOutliers(input, result); break;
    case AlarmType::Count: throw std::invalid_argument("AlarmType::Count is not a detector");
    }
    return result;
}

void MonitoringDetectors::checkPowerBalance(const MonitoringInput& input, MonitoringResult& result) const {
    if (input.measuredTotalPowerHistory.empty()) return;
    double receivedPower = 0.0;
    bool hasReceivedPower = false;
    for (const auto& turbine : input.turbines) {
        if (isRecent(turbine.powerTimeMs, input.currentTimeMs, powerMeasurementTimeoutMs) &&
            !turbine.powerHistory.empty()) {
            receivedPower += average(turbine.powerHistory);
            hasReceivedPower = true;
        }
    }
    if (!hasReceivedPower) return;
    const double measuredPower = average(input.measuredTotalPowerHistory);
    constexpr double thresholdW = 10e6;
    if (std::abs(receivedPower - measuredPower) > thresholdW) {
        addEvidence(result, AlarmType::PowerGeneratedVsReceived, 0, receivedPower, measuredPower,
                    thresholdW, input.currentTimeMs);
    }
}

void MonitoringDetectors::checkPowerTracking(const MonitoringInput& input, MonitoringResult& result) {
    for (std::size_t index = 0; index < turbineCount_; ++index) {
        const auto& turbine = input.turbines[index];
        double expectedPower = 0.0;
        double availablePower = 0.0;
        if (!expectedPowerForController(turbine, input, expectedPower, availablePower)) {
            powerMismatchStartTime_[index] = 0;
            lastExpectedPower_[index] = -1.0;
            expectedPowerHistory_[index].clear();
            continue;
        }

        appendHistory(expectedPowerHistory_[index], expectedPower);
        const double expectedAverage = average(expectedPowerHistory_[index]);
        const double tolerance = std::max(powerTrackingAbsoluteToleranceW,
            powerTrackingRelativeTolerance * std::max(expectedAverage, 0.0));
        if (lastExpectedPower_[index] < 0.0 || std::abs(expectedAverage - lastExpectedPower_[index]) > tolerance) {
            powerMismatchStartTime_[index] = 0;
            lastExpectedPower_[index] = expectedAverage;
        }

        const double measuredAverage = isRecent(turbine.powerTimeMs, input.currentTimeMs, powerMeasurementTimeoutMs) &&
            !turbine.powerHistory.empty() ? average(turbine.powerHistory) : 0.0;
        if (std::abs(measuredAverage - expectedAverage) <= tolerance) {
            powerMismatchStartTime_[index] = 0;
            continue;
        }
        if (powerMismatchStartTime_[index] == 0) {
            powerMismatchStartTime_[index] = input.currentTimeMs;
            continue;
        }
        if (input.currentTimeMs - powerMismatchStartTime_[index] >= powerTrackingGracePeriodMs) {
            addEvidence(result, AlarmType::MeasuredPowerVsExpected, static_cast<int>(index + 1),
                        measuredAverage, expectedAverage, tolerance, input.currentTimeMs);
        }
    }
}

void MonitoringDetectors::checkOrientation(const MonitoringInput& input, MonitoringResult& result) {
    for (std::size_t index = 0; index < turbineCount_; ++index) {
        const auto& turbine = input.turbines[index];
        const bool freshMeasurement = isRecent(turbine.yawTimeMs, input.currentTimeMs, yawMeasurementTimeoutMs);
        if (lastOrientationPredictionTime_[index] == 0) {
            if (!freshMeasurement) continue;
            orientation_[index] = normalizeAngleDeg(static_cast<float>(turbine.yaw));
            lastOrientationPredictionTime_[index] = input.currentTimeMs;
            lastYawMeasurementTime_[index] = turbine.yawTimeMs;
            continue;
        }

        const float elapsedSeconds = static_cast<float>(input.currentTimeMs - lastOrientationPredictionTime_[index]) / 1000.0F;
        const float predicted = predictYawAtRate(orientation_[index], turbine.yawSetpoint,
            static_cast<float>(sc::TurbineParameters::yawingRate), elapsedSeconds);
        lastOrientationPredictionTime_[index] = input.currentTimeMs;
        if (freshMeasurement && turbine.yawTimeMs > lastYawMeasurementTime_[index]) {
            const float error = angularDistanceDeg(predicted, static_cast<float>(turbine.yaw));
            if (error > orientationThresholdDeg) {
                addEvidence(result, AlarmType::OrientationMisalignment, static_cast<int>(index + 1),
                            turbine.yaw, predicted, orientationThresholdDeg, input.currentTimeMs);
            }
            lastYawMeasurementTime_[index] = turbine.yawTimeMs;
            const float signedError = shortestAngleDiffDeg(predicted, static_cast<float>(turbine.yaw));
            orientation_[index] = normalizeAngleDeg(predicted + orientationObserverGain * signedError);
        } else {
            orientation_[index] = predicted;
        }
    }
}

void MonitoringDetectors::checkPowerTorqueRotorSpeed(const MonitoringInput& input, MonitoringResult& result) const {
    constexpr uint64_t maximumTimestampDifferenceMs = 1000;
    constexpr double thresholdW = 4e5;
    for (std::size_t index = 0; index < turbineCount_; ++index) {
        const auto& turbine = input.turbines[index];
        const uint64_t difference = turbine.powerTimeMs > turbine.rotorSpeedTimeMs
            ? turbine.powerTimeMs - turbine.rotorSpeedTimeMs : turbine.rotorSpeedTimeMs - turbine.powerTimeMs;
        if (difference > maximumTimestampDifferenceMs) continue;
        const double expectedPower = sc::TurbineParameters::generatorEfficiency * turbine.rotorSpeed *
            2.0 * pi / 60.0 * turbine.generatorTorque * sc::TurbineParameters::gearboxRatio;
        if (std::abs(turbine.power - expectedPower) > thresholdW) {
            addEvidence(result, AlarmType::PowerTorqueRotorSpeed, static_cast<int>(index + 1),
                        turbine.power, expectedPower, thresholdW, input.currentTimeMs);
        }
    }
}

void MonitoringDetectors::checkWindDirection(const MonitoringInput& input, MonitoringResult& result) const {
    constexpr double thresholdDeg = 25.0;
    for (std::size_t index = 0; index < turbineCount_; ++index) {
        const double difference = angularDistanceDeg(static_cast<float>(input.turbines[index].windDirection),
                                                     input.farmWindDirection);
        if (difference > thresholdDeg) {
            addEvidence(result, AlarmType::WindDirection, static_cast<int>(index + 1),
                        input.turbines[index].windDirection, input.farmWindDirection, thresholdDeg,
                        input.currentTimeMs);
        }
    }
}

void MonitoringDetectors::checkWindDirectionChange(const MonitoringInput& input, MonitoringResult& result) {
    for (std::size_t index = 0; index < turbineCount_; ++index) {
        const auto& history = input.turbines[index].windDirectionHistory;
        if (history.size() < 5) {
            windDirectionChangeStrikes_[index] = 0;
            continue;
        }
        const float priorMean = circularMeanDeg(history, history.size() - 1);
        const float newest = static_cast<float>(history.back());
        const float allMean = circularMeanDeg(history, history.size());
        double maximumDistance = 0.0;
        for (double value : history) {
            maximumDistance = std::max(maximumDistance,
                static_cast<double>(angularDistanceDeg(allMean, static_cast<float>(value))));
        }
        const double step = angularDistanceDeg(priorMean, newest);
        const bool suspicious = step > windDirectionStepThresholdDeg &&
                                maximumDistance > windDirectionRangeThresholdDeg;
        windDirectionChangeStrikes_[index] = suspicious ? windDirectionChangeStrikes_[index] + 1 : 0;
        if (windDirectionChangeStrikes_[index] >= windChangeRequiredStrikes) {
            addEvidence(result, AlarmType::WindDirectionChange, static_cast<int>(index + 1), newest,
                        priorMean, windDirectionStepThresholdDeg, input.currentTimeMs);
        }
    }
}

void MonitoringDetectors::checkWindSpeedChange(const MonitoringInput& input, MonitoringResult& result) {
    for (std::size_t index = 0; index < turbineCount_; ++index) {
        const auto& history = input.turbines[index].windSpeedHistory;
        if (history.size() < 5) {
            windSpeedChangeStrikes_[index] = 0;
            continue;
        }
        std::vector<double> priorValues(history.begin(), history.end() - 1);
        const double priorMedian = median(std::move(priorValues));
        const double step = std::abs(history.back() - priorMedian);
        const bool suspicious = step > windSpeedStepThresholdMs && linearRange(history) > windSpeedRangeThresholdMs;
        windSpeedChangeStrikes_[index] = suspicious ? windSpeedChangeStrikes_[index] + 1 : 0;
        if (windSpeedChangeStrikes_[index] >= windChangeRequiredStrikes) {
            addEvidence(result, AlarmType::WindSpeedChange, static_cast<int>(index + 1), history.back(),
                        priorMedian, windSpeedStepThresholdMs, input.currentTimeMs);
        }
    }
}

void MonitoringDetectors::checkTelemetryFreeze(const MonitoringInput& input, MonitoringResult& result) {
    const auto checkSignal = [&](std::size_t turbineIndex, FreezeSignal signal,
                                 const std::vector<double>& history, uint64_t timestampMs,
                                 double value, double minimumActiveValue, bool angular) {
        auto& startTime = freezeStartTime_[turbineIndex][signal];
        const double observedRange = angular ? angularSpreadDeg(history) : linearRange(history);
        const bool frozen = history.size() >= freezeWindowSamples &&
            isRecent(timestampMs, input.currentTimeMs, freezeMeasurementTimeoutMs) &&
            value > minimumActiveValue && observedRange <= 0.0;
        if (!frozen) {
            startTime = 0;
            return;
        }
        if (startTime == 0) {
            startTime = input.currentTimeMs;
            return;
        }
        if (input.currentTimeMs - startTime >= freezePersistenceMs) {
            addEvidence(result, AlarmType::TelemetryFreeze, static_cast<int>(turbineIndex + 1),
                        observedRange, 0.0, 0.0, input.currentTimeMs);
        }
    };

    for (std::size_t index = 0; index < turbineCount_; ++index) {
        const auto& turbine = input.turbines[index];
        checkSignal(index, FreezeWindSpeed, turbine.windSpeedHistory, turbine.windSpeedTimeMs,
                    turbine.windSpeed, 1.0, false);
        checkSignal(index, FreezeWindDirection, turbine.windDirectionHistory, turbine.windDirectionTimeMs,
                    turbine.windDirection, 0.0, true);
        checkSignal(index, FreezeRotorSpeed, turbine.rotorSpeedHistory, turbine.rotorSpeedTimeMs,
                    turbine.rotorSpeed, 0.5, false);
        checkSignal(index, FreezePower, turbine.powerHistory, turbine.powerTimeMs,
                    turbine.power, 1000.0, false);
        checkSignal(index, FreezeTorque, turbine.generatorTorqueHistory, turbine.generatorTorqueTimeMs,
                    turbine.generatorTorque, 50.0, false);
    }
}

void MonitoringDetectors::checkDrivetrainResponse(const MonitoringInput& input, MonitoringResult& result) {
    for (std::size_t index = 0; index < turbineCount_; ++index) {
        const auto& turbine = input.turbines[index];
        if (turbine.commandedOff) {
            drivetrainUnderResponseStartTime_[index] = 0;
            continue;
        }
        const double availablePower = yawAdjustedAvailablePower(turbine, input);
        if (availablePower < drivetrainMinimumAerodynamicPowerW) {
            drivetrainUnderResponseStartTime_[index] = 0;
            continue;
        }
        double expectedPower = 0.0;
        if (turbine.maximumPowerMode) expectedPower = availablePower;
        else if (turbine.powerSetpoint >= 0.0F) expectedPower = std::min<double>(turbine.powerSetpoint, availablePower);

        const double expectedRpm = expectedRotorSpeedRpm(turbine, input);
        const double aerodynamicTorque = expectedGeneratorTorqueNm(availablePower, expectedRpm);
        const double commandedTorque = expectedGeneratorTorqueNm(expectedPower, expectedRpm);
        if (expectedRpm <= 0.0 || aerodynamicTorque <= 0.0) {
            drivetrainUnderResponseStartTime_[index] = 0;
            continue;
        }

        const bool hasRecentRpm = isRecent(turbine.rotorSpeedTimeMs, input.currentTimeMs, powerMeasurementTimeoutMs);
        const bool hasRecentTorque = isRecent(turbine.generatorTorqueTimeMs, input.currentTimeMs, powerMeasurementTimeoutMs);
        const bool rpmCollapsed = !hasRecentRpm || turbine.rotorSpeed < drivetrainCollapsedRpmFraction * expectedRpm;
        const bool torqueCollapsed = !hasRecentTorque || std::abs(turbine.generatorTorque) <
            std::max(drivetrainCollapsedTorqueNm, drivetrainCollapsedTorqueFraction * aerodynamicTorque);
        const bool aerodynamicCollapse = rpmCollapsed && torqueCollapsed;
        const bool rpmTooLow = !hasRecentRpm || turbine.rotorSpeed < drivetrainRpmLowFraction * expectedRpm;
        const bool torqueTooLow = expectedPower >= drivetrainMinimumExpectedPowerW && commandedTorque > 0.0 &&
            (!hasRecentTorque || turbine.generatorTorque < drivetrainTorqueLowFraction * commandedTorque);
        if (!aerodynamicCollapse && !rpmTooLow && !torqueTooLow) {
            drivetrainUnderResponseStartTime_[index] = 0;
            continue;
        }
        if (drivetrainUnderResponseStartTime_[index] == 0) {
            drivetrainUnderResponseStartTime_[index] = input.currentTimeMs;
            continue;
        }
        const uint64_t gracePeriod = aerodynamicCollapse ? drivetrainCollapseGracePeriodMs
                                                          : drivetrainUnderResponseGracePeriodMs;
        if (input.currentTimeMs - drivetrainUnderResponseStartTime_[index] >= gracePeriod) {
            addEvidence(result, AlarmType::DrivetrainUnderResponse, static_cast<int>(index + 1),
                        turbine.rotorSpeed, expectedRpm, drivetrainRpmLowFraction * expectedRpm,
                        input.currentTimeMs);
        }
    }
}

void MonitoringDetectors::checkStaticBounds(const MonitoringInput& input, MonitoringResult& result) const {
    const auto checkRange = [&](std::size_t index, uint64_t timestampMs, double value,
                                double minimum, double maximum) {
        if (!isRecent(timestampMs, input.currentTimeMs, staticBoundsMeasurementTimeoutMs)) return;
        if (value < minimum || value > maximum) {
            const double violatedBound = value < minimum ? minimum : maximum;
            addEvidence(result, AlarmType::StaticTelemetryBounds, static_cast<int>(index + 1),
                        value, violatedBound, 0.0, input.currentTimeMs);
        }
    };

    for (std::size_t index = 0; index < turbineCount_; ++index) {
        const auto& turbine = input.turbines[index];
        checkRange(index, turbine.windSpeedTimeMs, turbine.windSpeed, 0.0, staticBoundsWindSpeedMaxMs);
        checkRange(index, turbine.windDirectionTimeMs, turbine.windDirection, 0.0, 360.0);
        checkRange(index, turbine.yawTimeMs, turbine.yaw, 0.0, 360.0);
        if (isRecent(turbine.yawTimeMs, input.currentTimeMs, staticBoundsMeasurementTimeoutMs) &&
            input.farmWindSpeed >= sc::TurbineParameters::cutInWindSpeed) {
            const double error = angularDistanceDeg(static_cast<float>(turbine.yaw), input.farmWindDirection);
            if (error > staticBoundsOrientationWindowDeg) {
                addEvidence(result, AlarmType::StaticTelemetryBounds, static_cast<int>(index + 1),
                            turbine.yaw, input.farmWindDirection, staticBoundsOrientationWindowDeg,
                            input.currentTimeMs);
            }
        }
        checkRange(index, turbine.rotorSpeedTimeMs, turbine.rotorSpeed, 0.0, staticBoundsRpmMax);
        checkRange(index, turbine.powerTimeMs, turbine.power, staticBoundsPowerMinW, staticBoundsPowerMaxW);
        checkRange(index, turbine.generatorTorqueTimeMs, turbine.generatorTorque,
                   staticBoundsTorqueMinNm, staticBoundsTorqueMaxNm);
    }
}

void MonitoringDetectors::checkFleetPeerOutliers(const MonitoringInput& input, MonitoringResult& result) {
    if (turbineCount_ < 3) {
        std::fill(fleetPeerOutlierStartTime_.begin(), fleetPeerOutlierStartTime_.end(), 0);
        return;
    }
    const auto operatingCount = std::count_if(input.turbines.begin(), input.turbines.end(),
        [](const TurbineMonitoringInput& turbine) { return !turbine.commandedOff; });
    if (operatingCount != static_cast<std::ptrdiff_t>(turbineCount_) ||
        input.connectedTurbines < static_cast<int>(turbineCount_)) {
        std::fill(fleetPeerOutlierStartTime_.begin(), fleetPeerOutlierStartTime_.end(), 0);
        return;
    }

    std::vector<std::size_t> indices;
    std::vector<double> powerRatios;
    std::vector<double> rpmRatios;
    for (std::size_t index = 0; index < turbineCount_; ++index) {
        const auto& turbine = input.turbines[index];
        if (!isRecent(turbine.powerTimeMs, input.currentTimeMs, powerMeasurementTimeoutMs) ||
            !isRecent(turbine.rotorSpeedTimeMs, input.currentTimeMs, powerMeasurementTimeoutMs)) continue;
        double expectedPower = 0.0;
        double availablePower = 0.0;
        if (!expectedPowerForController(turbine, input, expectedPower, availablePower) ||
            expectedPower < fleetPeerMinimumExpectedPowerW) continue;
        const double expectedRpm = expectedRotorSpeedRpm(turbine, input);
        if (expectedRpm <= 0.0) continue;
        indices.push_back(index);
        powerRatios.push_back(turbine.power / expectedPower);
        rpmRatios.push_back(turbine.rotorSpeed / expectedRpm);
    }

    if (indices.size() < turbineCount_ - 2) {
        std::fill(fleetPeerOutlierStartTime_.begin(), fleetPeerOutlierStartTime_.end(), 0);
        return;
    }
    const double medianPowerRatio = median(powerRatios);
    const double medianRpmRatio = median(rpmRatios);
    const double powerThreshold = std::max(fleetPeerPowerRatioThreshold,
        4.0 * medianAbsoluteDeviation(powerRatios, medianPowerRatio));
    const double rpmThreshold = std::max(fleetPeerRpmRatioThreshold,
        4.0 * medianAbsoluteDeviation(rpmRatios, medianRpmRatio));
    std::vector<bool> hasMetric(turbineCount_, false);
    for (std::size_t metric = 0; metric < indices.size(); ++metric) {
        const std::size_t index = indices[metric];
        hasMetric[index] = true;
        const double powerDifference = std::abs(powerRatios[metric] - medianPowerRatio);
        const double rpmDifference = std::abs(rpmRatios[metric] - medianRpmRatio);
        if (powerDifference <= powerThreshold && rpmDifference <= rpmThreshold) {
            fleetPeerOutlierStartTime_[index] = 0;
            continue;
        }
        if (fleetPeerOutlierStartTime_[index] == 0) {
            fleetPeerOutlierStartTime_[index] = input.currentTimeMs;
            continue;
        }
        if (input.currentTimeMs - fleetPeerOutlierStartTime_[index] >= fleetPeerOutlierGracePeriodMs) {
            const bool powerIsWorse = powerDifference / powerThreshold >= rpmDifference / rpmThreshold;
            addEvidence(result, AlarmType::FleetPeerOutlier, static_cast<int>(index + 1),
                        powerIsWorse ? powerRatios[metric] : rpmRatios[metric],
                        powerIsWorse ? medianPowerRatio : medianRpmRatio,
                        powerIsWorse ? powerThreshold : rpmThreshold, input.currentTimeMs);
        }
    }
    for (std::size_t index = 0; index < turbineCount_; ++index) {
        if (!hasMetric[index]) fleetPeerOutlierStartTime_[index] = 0;
    }
}

} // namespace sc::application
