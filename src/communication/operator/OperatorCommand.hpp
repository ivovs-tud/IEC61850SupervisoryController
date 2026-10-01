#pragma once

#include <variant>

struct RequestedPowerCommand {
    float value{0.0F};
};

struct SimulationStateCommand {
    bool running{false};
};

using OperatorCommand = std::variant<RequestedPowerCommand, SimulationStateCommand>;
