#pragma once

#include <cstdint>

namespace AttackInterface {

enum class SignalType : uint32_t {
    WIND_SPEED = 0x01,
    WIND_DIRECTION = 0x02,
    TURBINE_STATUS = 0x03,
    POWER = 0x04,
    YAW_ANGLE = 0x05,
    ROTOR_SPEED = 0x06,
    PITCH_ANGLE = 0x07,
    YAW_SETPOINT = 0x08,
    POWER_SETPOINT = 0x09,
    OPERATION_COMMAND = 0x0A,
    GENERATOR_TORQUE = 0x10,
    NONE = 0xFE,
    ARRAY = 0xFF,
};

} // namespace AttackInterface
