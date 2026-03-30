#pragma once

#include "common/PeriodicTask.hpp"

// ---------------------------------------------------------------------------
// SignalProcessingTask – ADC / sensor signal processing.
// ---------------------------------------------------------------------------
class SignalProcessingTask : public PeriodicTask
{
public:
    explicit SignalProcessingTask(std::chrono::milliseconds period = std::chrono::milliseconds(1));

    void init();

protected:
    void execute() override;
};
