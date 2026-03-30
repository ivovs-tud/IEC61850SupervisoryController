#pragma once

#include "common/PeriodicTask.hpp"

// ---------------------------------------------------------------------------
// HmiTask – Human-Machine Interface update loop.
// ---------------------------------------------------------------------------
class HmiTask : public PeriodicTask
{
public:
    explicit HmiTask(std::chrono::milliseconds period = std::chrono::milliseconds(100));

protected:
    void execute() override;
};
