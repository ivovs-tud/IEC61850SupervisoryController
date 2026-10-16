#pragma once

#include <cstdint>

// Native fixed-size record retained for the existing data-historian TCP input.
struct DataHistorianRecord {
    uint32_t turbineId;
    uint64_t unixTime;
    float yawAngle;
    float yawSetpoint;
    float power;
    float powerSetpoint;
    float windSpeed;
    float windDirection;
    float rotorSpeed;
    float pitchAngle;
    float pitchSetpoint;
};
