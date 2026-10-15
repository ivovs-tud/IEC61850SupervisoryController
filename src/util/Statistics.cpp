#include "sc/util/Statistics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <utility>

namespace sc::util {

double mean(const std::vector<double>& values)
{
    if (values.empty()) {
        return 0.0;
    }

    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
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

double medianAbsoluteDeviation(std::vector<double> values, double center)
{
    for (double& value : values) {
        value = std::abs(value - center);
    }

    return median(std::move(values));
}

double valueRange(const std::vector<double>& values)
{
    if (values.empty()) {
        return 0.0;
    }

    const auto [minimum, maximum] = std::minmax_element(values.begin(), values.end());
    return *maximum - *minimum;
}

} // namespace sc::util
