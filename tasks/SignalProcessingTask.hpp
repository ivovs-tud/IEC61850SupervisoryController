#pragma once

#include "common/PeriodicTask.hpp"

// Aggregates turbine measurements into farm-level values.
class SignalProcessingTask : public PeriodicTask {
public:
    explicit SignalProcessingTask(std::chrono::milliseconds period = std::chrono::milliseconds(1));
    ~SignalProcessingTask() override { stop(); }

protected:
    void execute() override;
};
