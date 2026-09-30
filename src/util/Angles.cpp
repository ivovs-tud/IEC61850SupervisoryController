#include "sc/util/Angles.hpp"

#include <algorithm>
#include <cmath>

namespace sc::util {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr float fullRotationDegrees = 360.0F;

} // namespace

float normalizeAngleDegrees(float angle) {
    float normalized = std::fmod(angle, fullRotationDegrees);
    if (normalized < 0.0F) {
        normalized += fullRotationDegrees;
    }

    if (std::abs(normalized) < 1e-5F || std::abs(normalized - fullRotationDegrees) < 1e-5F) {
        return 0.0F;
    }

    return normalized;
}

float roundAngleDegrees(float angle) {
    return normalizeAngleDegrees(std::round(angle));
}

float signedAngleDifferenceDegrees(float from, float to) {
    float difference = normalizeAngleDegrees(to) - normalizeAngleDegrees(from);
    if (difference > 180.0F) {
        difference -= fullRotationDegrees;
    }

    if (difference < -180.0F) {
        difference += fullRotationDegrees;
    }

    return difference;
}

float angularDistanceDegrees(float first, float second) {
    return std::abs(signedAngleDifferenceDegrees(first, second));
}

float circularMeanDegrees(const std::vector<double>& values, std::size_t count) {
    double sinSum = 0.0;
    double cosSum = 0.0;
    const std::size_t sampleCount = std::min(count, values.size());
    for (std::size_t index = 0; index < sampleCount; ++index) {
        const double radians = values[index] * pi / 180.0;
        sinSum += std::sin(radians);
        cosSum += std::cos(radians);
    }
    if (sinSum == 0.0 && cosSum == 0.0) return 0.0F;
    return normalizeAngleDegrees(static_cast<float>(std::atan2(sinSum, cosSum) * 180.0 / pi));
}

double angularSpreadDegrees(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    
    const float center = circularMeanDegrees(values, values.size());
    double spread = 0.0;
    for (double value : values) {
        spread = std::max(spread, static_cast<double>(angularDistanceDegrees(center, static_cast<float>(value))));
    }

    return spread;
}

float blendAnglesDegrees(float previous, float current, float updateWeight) {
    return normalizeAngleDegrees(previous + updateWeight * signedAngleDifferenceDegrees(previous, current));
}

float moveTowardsAngleDegrees(float current, float target, float rateDegreesPerSecond, float elapsedSeconds) {
    const float difference = signedAngleDifferenceDegrees(current, target);
    const float maximumStep = std::max(0.0F, rateDegreesPerSecond * elapsedSeconds);
    return normalizeAngleDegrees(current + std::clamp(difference, -maximumStep, maximumStep));
}

} // namespace sc::util
