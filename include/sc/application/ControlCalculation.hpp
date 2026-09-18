#pragma once

#include <vector>

#include "sc/application/YawLut.hpp"

namespace sc::application {

struct ControlInputs {
    int turbineCount{0};
    float requestedReferencePower{0.0F};
    float windSpeed{0.0F};
    float windDirection{0.0F};
    bool yawSteeringEnabled{false};
};

struct ControlSetpoints {
    std::vector<float> turbinePower;
    std::vector<float> turbineYaw;
};

ControlSetpoints calculateControlSetpoints(const ControlInputs& inputs, const YawLut& yawLut);

} // namespace sc::application
