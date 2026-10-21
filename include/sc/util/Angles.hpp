#pragma once

#include <cstddef>
#include <vector>

namespace sc::util {

float normalizeAngleDegrees(float angle);
float roundAngleDegrees(float angle);
float signedAngleDifferenceDegrees(float from, float to);
float angularDistanceDegrees(float first, float second);
float circularMeanDegrees(const std::vector<double>& values, std::size_t count);
double angularSpreadDegrees(const std::vector<double>& values);
float blendAnglesDegrees(float previous, float current, float updateWeight);
float moveTowardsAngleDegrees(float current, float target, float rateDegreesPerSecond, float elapsedSeconds);

} // namespace sc::util
