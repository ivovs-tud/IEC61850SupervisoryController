#include "sc/application/ControlCalculation.hpp"
#include "sc/util/Angles.hpp"

#include <cstddef>

namespace sc::application {

ControlSetpoints calculateControlSetpoints(const ControlInputs& inputs, const YawLut& yawLut) {
    const auto turbineCount = static_cast<std::size_t>(inputs.turbineCount);
    ControlSetpoints setpoints{
        std::vector<float>(turbineCount, -1.0F),
        std::vector<float>(turbineCount, sc::util::roundAngleDegrees(inputs.windDirection))};

    if (inputs.turbineCount == 0) {
        return setpoints;
    }

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
