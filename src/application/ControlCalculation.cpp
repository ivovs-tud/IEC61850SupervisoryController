#include "sc/application/ControlCalculation.hpp"
#include "sc/util/Angles.hpp"

#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace sc::application {

ControlSetpoints calculateControlSetpoints(const ControlInputs& inputs, const YawLut& yawLut) {
    if (inputs.turbineCount < 0) {
        throw std::invalid_argument("control calculation turbine count must not be negative");
    }
    const auto turbineCount = static_cast<std::size_t>(inputs.turbineCount);
    if (turbineCount == 0) return {{}, {}};
    if (!std::isfinite(inputs.requestedReferencePower) || !std::isfinite(inputs.windSpeed) ||
        !std::isfinite(inputs.windDirection)) {
        throw std::invalid_argument("control calculation inputs must be finite");
    }
    ControlSetpoints setpoints{
        std::vector<float>(turbineCount, -1.0F),
        std::vector<float>(turbineCount, sc::util::roundAngleDegrees(inputs.windDirection))};

    if (inputs.yawSteeringEnabled) {
        const auto yawOffsets = yawLut.lookup(inputs.windSpeed, inputs.windDirection);
        setpoints.turbineYaw.clear();
        setpoints.turbineYaw.reserve(yawOffsets.size());
        
        for (const float offset : yawOffsets) {
            setpoints.turbineYaw.push_back(sc::util::roundAngleDegrees(inputs.windDirection - offset));
        }
    }

    // Negative references are per-turbine commands/sentinels, not farm totals.
    /* TODO: Add option to to adaptive power sharing, i.e. 
        P_{i,\mathrm{ref}} =  \frac{P_{i,\mathrm{avail}}}{\sum_{i=1}^{N_{\mathrm{WT}}} P_{i, \mathrm{avail}}} \cdot P_{\mathrm{wf,ref}}
    */
    const float turbinePower = inputs.requestedReferencePower < 0.0F ? inputs.requestedReferencePower : static_cast<float>(inputs.requestedReferencePower / inputs.turbineCount);
    setpoints.turbinePower.assign(turbineCount, turbinePower);

    return setpoints;
}

} // namespace sc::application
