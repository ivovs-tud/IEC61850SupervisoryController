#pragma once

#include "sc/application/PowerSharingMode.hpp"
#include "sc/application/YawLut.hpp"

#include <vector>

namespace sc::application {

struct ControlInputs {
    int turbineCount{0};
    float requestedReferencePower{0.0F};
    float windSpeed{0.0F};
    float windDirection{0.0F};
    bool yawSteeringEnabled{false};
    PowerSharingMode powerSharingMode{PowerSharingMode::EQUAL};
    std::vector<double> availablePower;
};

struct ControlSetpoints {
    std::vector<float> turbinePower;
    std::vector<float> turbineYaw;
};

ControlSetpoints calculateControlSetpoints(const ControlInputs& inputs, const YawLut& yawLut);

} // namespace sc::application
