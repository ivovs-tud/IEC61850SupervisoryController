#pragma once

#include "sc/application/SignalProcessing.hpp"
#include "sc/runtime/PeriodicTask.hpp"

// Aggregates turbine measurements into farm-level values.
class SignalProcessingTask : public PeriodicTask {
public:
    explicit SignalProcessingTask(
        std::chrono::milliseconds period = std::chrono::milliseconds(1),
        sc::application::SignalProcessingConfig config = {});
    ~SignalProcessingTask() override { stop(); }

protected:
    void execute() override;

private:
    sc::application::SignalProcessingConfig config_;
};
