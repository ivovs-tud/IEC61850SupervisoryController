#pragma once

#include <vector>

namespace sc::util {

double mean(const std::vector<double>& values);
double median(std::vector<double> values);
double medianAbsoluteDeviation(std::vector<double> values, double center);
double valueRange(const std::vector<double>& values);

} // namespace sc::util
