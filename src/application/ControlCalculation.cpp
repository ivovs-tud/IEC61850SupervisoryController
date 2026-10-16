#include "sc/application/ControlCalculation.hpp"

#include "sc/util/Angles.hpp"

#include <cmath>
#include <cstddef>
#include <numeric>
#include <stdexcept>

namespace sc::application {

void equalPowerSharing(const ControlInputs& inputs, ControlSetpoints& setpoints)
{
    const float turbinePower = inputs.requestedReferencePower / static_cast<float>(inputs.turbineCount);
    setpoints.turbinePower.assign(inputs.turbineCount, turbinePower);
}

void availablePowerSharing(const ControlInputs& inputs, ControlSetpoints& setpoints)
{
    if (inputs.availablePower.size() != static_cast<std::size_t>(inputs.turbineCount)) {
        throw std::invalid_argument("available-power count does not match turbine count");
    }
    for (const double availablePower : inputs.availablePower) {
        if (!std::isfinite(availablePower) || availablePower < 0.0) {
            throw std::invalid_argument("available power must be finite and non-negative");
        }
    }
    const double totalAvailablePower = std::accumulate(inputs.availablePower.begin(), inputs.availablePower.end(), 0.0);
    if (totalAvailablePower > 0.0) {
        for (std::size_t index = 0; index < static_cast<std::size_t>(inputs.turbineCount); ++index) {
            setpoints.turbinePower[index] =
                static_cast<float>(inputs.requestedReferencePower * inputs.availablePower[index] / totalAvailablePower);
        }
    } else {
        equalPowerSharing(inputs, setpoints);
    }
}

ControlSetpoints calculateControlSetpoints(const ControlInputs& inputs, const YawLut& yawLut)
{
    if (inputs.turbineCount < 0) {
        throw std::invalid_argument("control calculation turbine count must not be negative");
    }
    const auto turbineCount = static_cast<std::size_t>(inputs.turbineCount);
    if (turbineCount == 0)
        return {{}, {}};
    if (!std::isfinite(inputs.requestedReferencePower) || !std::isfinite(inputs.windSpeed) || !std::isfinite(inputs.windDirection)) {
        throw std::invalid_argument("control calculation inputs must be finite");
    }

    ControlSetpoints setpoints{std::vector<float>(turbineCount, -1.0F),
                               std::vector<float>(turbineCount, sc::util::roundAngleDegrees(inputs.windDirection))};

    if (inputs.yawSteeringEnabled) {
        const auto yawOffsets = yawLut.lookup(inputs.windSpeed, inputs.windDirection);
        setpoints.turbineYaw.clear();
        setpoints.turbineYaw.reserve(yawOffsets.size());

        for (const float offset : yawOffsets) {
            setpoints.turbineYaw.push_back(sc::util::roundAngleDegrees(inputs.windDirection - offset));
        }
    }

    if (inputs.requestedReferencePower < 0.0F) {
        // Negative references are per-turbine commands/sentinels, not farm totals.
        setpoints.turbinePower.assign(turbineCount, inputs.requestedReferencePower);
        return setpoints;
    }

    switch (inputs.powerSharingMode) {
    case PowerSharingMode::EQUAL:
        equalPowerSharing(inputs, setpoints);
        break;
    case PowerSharingMode::AVAILABLE_POWER:
        availablePowerSharing(inputs, setpoints);
        break;
    default:
        throw std::invalid_argument("unknown power-sharing mode");
    }

    return setpoints;
}

} // namespace sc::application
