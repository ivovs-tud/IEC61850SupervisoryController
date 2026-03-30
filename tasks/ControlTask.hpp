#pragma once

#include "common/PeriodicTask.hpp"

// ---------------------------------------------------------------------------
// ControlTask – closed-loop control algorithm.
// ---------------------------------------------------------------------------
class ControlTask : public PeriodicTask
{
public:
    explicit ControlTask(int numTurbines, std::chrono::milliseconds period = std::chrono::milliseconds(10));

protected:
    void execute() override;
    void onStop()  override;  // stops socket servers after the loop exits

private:
    int numTurbines_;
};
