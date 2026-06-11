#pragma once

#include "common/PeriodicTask.hpp"

/**
 * Computes farm-level aggregate signals from latest per-turbine measurements.
 *
 * At the moment this task derives total power, farm wind direction, and a
 * representative farm wind speed. It writes only processed values back to
 * GlobalData; raw measurements remain owned by the communication layer.
 */
class SignalProcessingTask : public PeriodicTask {
public:
    explicit SignalProcessingTask(std::chrono::milliseconds period = std::chrono::milliseconds(1));

    void init();

protected:
    void execute() override;
};
