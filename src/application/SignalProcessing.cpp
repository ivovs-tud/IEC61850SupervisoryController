#include "sc/application/SignalProcessing.hpp"

#include "sc/model/TurbineParameters.hpp"
#include "sc/util/Angles.hpp"
#include "sc/util/Time.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <numeric>
#include <limits>
#include <stdexcept>

namespace sc::application {
namespace {

constexpr double pi = 3.14159265358979323846;

} // namespace

double calculateAvailablePower(double windSpeed)
{
    if (!std::isfinite(windSpeed))
        return 0.0;
    if (windSpeed < sc::TurbineParameters::cutInWindSpeed || windSpeed >= sc::TurbineParameters::cutOutWindSpeed) {
        return 0.0;
    }

    const double rotorRadius = sc::TurbineParameters::rotorDiameter / 2.0;
    const double sweptArea = pi * rotorRadius * rotorRadius;
    const double aerodynamicPower = 0.5 * sc::TurbineParameters::airDensity * sweptArea * sc::TurbineParameters::optimalPowerCoefficient *
                                    windSpeed * windSpeed * windSpeed;
    return std::clamp(aerodynamicPower, 0.0, sc::TurbineParameters::ratedPower);
}

double calculateExpectedPower(double windSpeed, double windDirection, double yaw, float powerSetpoint,
                              bool maximumPowerMode, bool commandedOff)
{
    if (commandedOff)
        return 0.0;
    if (!std::isfinite(windSpeed) || !std::isfinite(windDirection) || !std::isfinite(yaw) ||
        !std::isfinite(powerSetpoint) || (!maximumPowerMode && powerSetpoint < 0.0F))
        return std::numeric_limits<double>::quiet_NaN();
    if (windSpeed < sc::TurbineParameters::cutInWindSpeed || windSpeed >= sc::TurbineParameters::cutOutWindSpeed)
        return 0.0;
    const double radius = sc::TurbineParameters::rotorDiameter / 2.0;
    const double aerodynamicPower = 0.5 * sc::TurbineParameters::generatorEfficiency * sc::TurbineParameters::airDensity *
                                    pi * radius * radius * sc::TurbineParameters::optimalPowerCoefficient *
                                    windSpeed * windSpeed * windSpeed;
    const double yawError = sc::util::angularDistanceDegrees(static_cast<float>(windDirection), static_cast<float>(yaw));
    const double cosine = std::max(0.0, std::cos(yawError * pi / 180.0));
    const double available = std::min(aerodynamicPower * cosine * cosine * cosine, sc::TurbineParameters::ratedPower);
    return maximumPowerMode ? available : std::min(static_cast<double>(powerSetpoint), available);
}

SignalProcessingResult processSignals(const SignalProcessingInput& input, const SignalProcessingConfig& config)
{
    if (!std::isfinite(input.previousWindSpeed) || !std::isfinite(input.previousWindDirection) ||
        !std::isfinite(config.windSpeedUpdateWeight) || !std::isfinite(config.windDirectionUpdateWeight) ||
        config.windSpeedUpdateWeight < 0.0F || config.windSpeedUpdateWeight > 1.0F || config.windDirectionUpdateWeight < 0.0F ||
        config.windDirectionUpdateWeight > 1.0F || config.windSpeedSampleCount == 0 || !std::isfinite(config.powerUpdateWeight) ||
        config.powerUpdateWeight <= 0.0F || config.powerUpdateWeight > 1.0F) {
        throw std::invalid_argument("signal-processing state and configuration must be valid");
    }
    const bool hasPreviousTurbineWind = !input.previousFilteredWindSpeeds.empty() || !input.previousFilteredWindSpeedTimeMs.empty();
    if (hasPreviousTurbineWind && (input.previousFilteredWindSpeeds.size() != input.turbines.size() ||
                                   input.previousFilteredWindSpeedTimeMs.size() != input.turbines.size())) {
        throw std::invalid_argument("previous turbine wind-speed state does not match turbine count");
    }
    const bool hasPreviousPower = !input.previousFilteredPowers.empty() || !input.previousFilteredPowerTimeMs.empty();
    if (hasPreviousPower && (input.previousFilteredPowers.size() != input.turbines.size() ||
                               input.previousFilteredPowerTimeMs.size() != input.turbines.size())) {
        throw std::invalid_argument("previous turbine power state does not match turbine count");
    }
    SignalProcessingResult result;
    result.filteredPowers = hasPreviousPower ? input.previousFilteredPowers : std::vector<double>(input.turbines.size(), 0.0);
    result.filteredPowerTimeMs = hasPreviousPower ? input.previousFilteredPowerTimeMs : std::vector<uint64_t>(input.turbines.size(), 0);
    result.availablePower.reserve(input.turbines.size());
    result.filteredWindSpeeds.assign(input.turbines.size(), 0.0);
    result.filteredWindSpeedTimeMs.assign(input.turbines.size(), 0);
    if (hasPreviousTurbineWind) {
        result.filteredWindSpeeds = input.previousFilteredWindSpeeds;
        result.filteredWindSpeedTimeMs = input.previousFilteredWindSpeedTimeMs;
    }
    result.windSpeed = input.previousWindSpeed;
    result.windDirection = sc::util::normalizeAngleDegrees(input.previousWindDirection);

    std::vector<double> validWindSpeeds;
    validWindSpeeds.reserve(input.turbines.size());
    double directionSinSum = 0.0;
    double directionCosSum = 0.0;
    std::size_t directionCount = 0;

    for (std::size_t index = 0; index < input.turbines.size(); ++index) {
        const auto& turbine = input.turbines[index];
        const bool connected =
            sc::util::isTimestampRecent(turbine.newestMeasurementTimeMs, input.currentTimeMs, config.measurementTimeoutMs);
        const bool windSpeedFresh = std::isfinite(turbine.windSpeed) &&
                                    sc::util::isTimestampRecent(turbine.windSpeedTimeMs, input.currentTimeMs, config.measurementTimeoutMs);
        const bool windDirectionFresh =
            std::isfinite(turbine.windDirection) &&
            sc::util::isTimestampRecent(turbine.windDirectionTimeMs, input.currentTimeMs, config.measurementTimeoutMs);
        const bool powerFresh = sc::util::isTimestampRecent(turbine.powerTimeMs, input.currentTimeMs, config.measurementTimeoutMs);

        if (connected) {
            ++result.connectedTurbines;
        }
        if (powerFresh && std::isfinite(turbine.receivedPower)) {
            result.totalReceivedPower += turbine.receivedPower;
            // Filter received turbine telemetry, including any attack-interface overrides.
            const bool previousIsFresh =
                std::isfinite(result.filteredPowers[index]) &&
                sc::util::isTimestampRecent(result.filteredPowerTimeMs[index], input.currentTimeMs, config.measurementTimeoutMs);
            if (!previousIsFresh) {
                result.filteredPowers[index] = turbine.receivedPower;
                result.filteredPowerTimeMs[index] = turbine.powerTimeMs;
            } else if (turbine.powerTimeMs > result.filteredPowerTimeMs[index]) {
                result.filteredPowers[index] += config.powerUpdateWeight * (turbine.receivedPower - result.filteredPowers[index]);
                result.filteredPowerTimeMs[index] = turbine.powerTimeMs;
            }
        }
        if (powerFresh && std::isfinite(turbine.measuredPower)) {
            result.totalMeasuredPower += turbine.measuredPower;
        }
        if (windSpeedFresh) {
            const bool previousIsFresh =
                hasPreviousTurbineWind &&
                sc::util::isTimestampRecent(input.previousFilteredWindSpeedTimeMs[index], input.currentTimeMs, config.measurementTimeoutMs);
            result.filteredWindSpeeds[index] = previousIsFresh
                                                   ? (1.0 - config.windSpeedUpdateWeight) * input.previousFilteredWindSpeeds[index] +
                                                         config.windSpeedUpdateWeight * turbine.windSpeed
                                                   : turbine.windSpeed;
            result.filteredWindSpeedTimeMs[index] = turbine.windSpeedTimeMs;
        }
        const bool filteredWindSpeedFresh =
            std::isfinite(result.filteredWindSpeeds[index]) &&
            sc::util::isTimestampRecent(result.filteredWindSpeedTimeMs[index], input.currentTimeMs, config.measurementTimeoutMs);
        result.availablePower.push_back(filteredWindSpeedFresh ? calculateAvailablePower(result.filteredWindSpeeds[index]) : 0.0);

        if (!windSpeedFresh || turbine.windSpeed <= 0.0) {
            continue;
        }
        validWindSpeeds.push_back(turbine.windSpeed);

        if (windDirectionFresh) {
            const double radians = turbine.windDirection * pi / 180.0;
            directionSinSum += std::sin(radians);
            directionCosSum += std::cos(radians);
            ++directionCount;
        }
    }

    if (!validWindSpeeds.empty()) {
        const std::size_t sampleCount = std::min(config.windSpeedSampleCount, validWindSpeeds.size());
        std::partial_sort(validWindSpeeds.begin(), validWindSpeeds.begin() + static_cast<std::ptrdiff_t>(sampleCount),
                          validWindSpeeds.end(), std::greater<double>());
        const double windSpeedSum =
            std::accumulate(validWindSpeeds.begin(), validWindSpeeds.begin() + static_cast<std::ptrdiff_t>(sampleCount), 0.0);
        const float measuredWindSpeed = static_cast<float>(windSpeedSum / static_cast<double>(sampleCount));
        result.windSpeed =
            (1.0F - config.windSpeedUpdateWeight) * input.previousWindSpeed + config.windSpeedUpdateWeight * measuredWindSpeed;
    }

    if (directionCount > 0 && std::hypot(directionSinSum, directionCosSum) > 1e-9) {
        const float measuredDirection =
            sc::util::normalizeAngleDegrees(static_cast<float>(std::atan2(directionSinSum, directionCosSum) * 180.0 / pi));
        result.windDirection =
            sc::util::blendAnglesDegrees(input.previousWindDirection, measuredDirection, config.windDirectionUpdateWeight);
    }

    // Compute after updating farm wind so all consumers receive the same expectation.
    constexpr uint64_t expectedPowerMeasurementTimeoutMs = 20000;
    result.expectedPowers.assign(input.turbines.size(), 0.0);
    result.expectedPowerTimeMs.assign(input.turbines.size(), 0);
    for (std::size_t index = 0; index < input.turbines.size(); ++index) {
        const auto& turbine = input.turbines[index];
        const double windSpeed = sc::util::isTimestampRecent(result.filteredWindSpeedTimeMs[index], input.currentTimeMs,
                                                             expectedPowerMeasurementTimeoutMs)
                                     ? result.filteredWindSpeeds[index] : static_cast<double>(result.windSpeed);
        const double yaw = sc::util::isTimestampRecent(turbine.yawTimeMs, input.currentTimeMs, expectedPowerMeasurementTimeoutMs)
                               ? turbine.yaw : static_cast<double>(turbine.yawSetpoint);
        result.expectedPowers[index] = calculateExpectedPower(windSpeed, result.windDirection, yaw, turbine.powerSetpoint,
                                                             turbine.maximumPowerMode, turbine.commandedOff);
        if (std::isfinite(result.expectedPowers[index]))
            result.expectedPowerTimeMs[index] = input.currentTimeMs;
    }

    return result;
}

} // namespace sc::application
