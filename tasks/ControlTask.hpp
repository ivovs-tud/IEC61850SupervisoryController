#pragma once

#include <chrono>
#include <string>

#include "common/PeriodicTask.hpp"
#include "sc/application/YawLut.hpp"

// ---------------------------------------------------------------------------
// ControlTask – closed-loop control algorithm.
// ---------------------------------------------------------------------------
class ControlTask : public PeriodicTask
{
public:
    struct Config {
        int numTurbines;
        std::chrono::milliseconds period{std::chrono::milliseconds(10)};
        std::string yawLutCsvPath{"yaw_lut.csv"};
    };

    explicit ControlTask(Config config);
    ~ControlTask() override { stop(); }

protected:
    void execute() override;
    void onStop()  override;  // stops socket servers after the loop exits

private:
    sc::application::YawLut yawLut_;
    int numTurbines_;
};
