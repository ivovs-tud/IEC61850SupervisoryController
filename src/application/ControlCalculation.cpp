#include "sc/application/ControlCalculation.hpp"

#include <cmath>
#include <cstddef>

namespace sc::application {
namespace {

int roundedOrientation(float degrees) {
    constexpr long fullRotation = 360;
    long orientation = std::lround(degrees) % fullRotation;
    if (orientation < 0) {
        orientation += fullRotation;
    }
    return static_cast<int>(orientation);
}

} // namespace

ControlSetpoints calculateControlSetpoints(const ControlInputs& inputs, const YawLut& yawLut) {
    const auto turbineCount = static_cast<std::size_t>(inputs.turbineCount);
    ControlSetpoints setpoints{
        std::vector<float>(turbineCount, -1.0F),
        std::vector<int>(turbineCount, roundedOrientation(inputs.windDirection))};

    if (inputs.turbineCount == 0) {
        return setpoints;
    }

    if (inputs.yawSteeringEnabled) {
        const auto yawOffsets = yawLut.lookup(inputs.windSpeed, inputs.windDirection);
        setpoints.turbineYaw.clear();
        setpoints.turbineYaw.reserve(yawOffsets.size());
        for (const float offset : yawOffsets) {
            setpoints.turbineYaw.push_back(roundedOrientation(inputs.windDirection - offset));
        }
    }

    // Negative references are per-turbine commands/sentinels, not farm totals.
    const float turbinePower = inputs.requestedReferencePower < 0.0F
                                   ? inputs.requestedReferencePower
                                   : static_cast<float>(inputs.requestedReferencePower / inputs.turbineCount);
    setpoints.turbinePower.assign(turbineCount, turbinePower);

    return setpoints;
}

} // namespace sc::application
