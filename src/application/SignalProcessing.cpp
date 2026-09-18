#include "sc/application/SignalProcessing.hpp"
#include "sc/TurbineParameters.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <functional>
#include <numeric>

namespace sc::application {
namespace {

constexpr double pi = 3.14159265358979323846;

bool isRecent(uint64_t timestampMs, uint64_t currentTimeMs, uint64_t timeoutMs) {
    return timestampMs != 0 &&
           (timestampMs >= currentTimeMs || currentTimeMs - timestampMs <= timeoutMs);
}

float normalizeDirection(float direction) {
    constexpr float fullRotation = 360.0F;
    float normalized = std::fmod(direction, fullRotation);
    if (normalized < 0.0F) {
        normalized += fullRotation;
    }
    if (std::abs(normalized) < 1e-5F || std::abs(normalized - fullRotation) < 1e-5F) {
        return 0.0F;
    }
    return normalized;
}

float blendDirection(float previous, float current, float updateWeight) {
    const float difference = std::remainder(current - previous, 360.0F);
    return normalizeDirection(previous + updateWeight * difference);
}

} // namespace

double calculateAvailablePower(double windSpeed) {
    if (windSpeed < sc::TurbineParameters::cutInWindSpeed || windSpeed >= sc::TurbineParameters::cutOutWindSpeed) {
        return 0.0;
    }

    const double rotorRadius = sc::TurbineParameters::rotorDiameter / 2.0;
    const double sweptArea = pi * rotorRadius * rotorRadius;
    const double aerodynamicPower = 0.5 * sc::TurbineParameters::airDensity * sweptArea *
                                    sc::TurbineParameters::optimalPowerCoefficient *
                                    windSpeed * windSpeed * windSpeed;
    return std::clamp(aerodynamicPower, 0.0, sc::TurbineParameters::ratedPower);
}

SignalProcessingResult processSignals(const SignalProcessingInput& input,
                                      const SignalProcessingConfig& config) {
    SignalProcessingResult result;
    result.availablePower.reserve(input.turbines.size());
    result.windSpeed = input.previousWindSpeed;
    result.windDirection = normalizeDirection(input.previousWindDirection);

    std::vector<double> validWindSpeeds;
    validWindSpeeds.reserve(input.turbines.size());
    double directionSinSum = 0.0;
    double directionCosSum = 0.0;
    std::size_t directionCount = 0;

    for (const auto& turbine : input.turbines) {
        const bool connected = isRecent(turbine.newestMeasurementTimeMs,
                                        input.currentTimeMs,
                                        config.measurementTimeoutMs);
        const bool windSpeedFresh = isRecent(turbine.windSpeedTimeMs,
                                             input.currentTimeMs,
                                             config.measurementTimeoutMs);
        const bool windDirectionFresh = isRecent(turbine.windDirectionTimeMs,
                                                 input.currentTimeMs,
                                                 config.measurementTimeoutMs);

        if (connected) {
            ++result.connectedTurbines;
        }
        result.totalReceivedPower += turbine.receivedPower;
        result.totalMeasuredPower += turbine.measuredPower;
        result.availablePower.push_back(windSpeedFresh
                                            ? calculateAvailablePower(turbine.windSpeed)
                                            : 0.0);

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
        constexpr std::size_t maximumWindSpeedSamples = 3;
        const std::size_t sampleCount = std::min(maximumWindSpeedSamples, validWindSpeeds.size());
        std::partial_sort(validWindSpeeds.begin(),
                          validWindSpeeds.begin() + static_cast<std::ptrdiff_t>(sampleCount),
                          validWindSpeeds.end(),
                          std::greater<double>());
        const double windSpeedSum = std::accumulate(validWindSpeeds.begin(),
                                                    validWindSpeeds.begin() + static_cast<std::ptrdiff_t>(sampleCount),
                                                    0.0);
        const float measuredWindSpeed = static_cast<float>(windSpeedSum / static_cast<double>(sampleCount));
        result.windSpeed = (1.0F - config.windSpeedUpdateWeight) * input.previousWindSpeed +
                           config.windSpeedUpdateWeight * measuredWindSpeed;
    }

    if (directionCount > 0 && std::hypot(directionSinSum, directionCosSum) > 1e-9) {
        const float measuredDirection = normalizeDirection(
            static_cast<float>(std::atan2(directionSinSum, directionCosSum) * 180.0 / pi));
        result.windDirection = blendDirection(input.previousWindDirection,
                                              measuredDirection,
                                              config.windDirectionUpdateWeight);
    }

    return result;
}

} // namespace sc::application
