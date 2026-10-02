#include "sc/application/SignalProcessing.hpp"

#include "sc/model/TurbineParameters.hpp"
#include "sc/util/Angles.hpp"
#include "sc/util/Time.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <numeric>
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

SignalProcessingResult processSignals(const SignalProcessingInput& input, const SignalProcessingConfig& config)
{
    if (!std::isfinite(input.previousWindSpeed) || !std::isfinite(input.previousWindDirection) ||
        !std::isfinite(config.windSpeedUpdateWeight) || !std::isfinite(config.windDirectionUpdateWeight) ||
        config.windSpeedUpdateWeight < 0.0F || config.windSpeedUpdateWeight > 1.0F || config.windDirectionUpdateWeight < 0.0F ||
        config.windDirectionUpdateWeight > 1.0F || config.windSpeedSampleCount == 0) {
        throw std::invalid_argument("signal-processing state and configuration must be valid");
    }
    const bool hasPreviousTurbineWind = !input.previousFilteredWindSpeeds.empty() || !input.previousFilteredWindSpeedTimeMs.empty();
    if (hasPreviousTurbineWind && (input.previousFilteredWindSpeeds.size() != input.turbines.size() ||
                                   input.previousFilteredWindSpeedTimeMs.size() != input.turbines.size())) {
        throw std::invalid_argument("previous turbine wind-speed state does not match turbine count");
    }
    SignalProcessingResult result;
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

    return result;
}

} // namespace sc::application
