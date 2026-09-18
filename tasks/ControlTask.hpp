#pragma once

#include <chrono>
#include <string>

#include "common/PeriodicTask.hpp"
#include "sc/application/YawLut.hpp"

/// Periodic adapter for the pure control calculation
class ControlTask : public PeriodicTask
{
public:
    /// Startup dependencies and schedule
    struct Config {
        int numTurbines;
        std::chrono::milliseconds period{std::chrono::milliseconds(10)};
        std::string yawLutCsvPath{"yaw_lut.csv"};
    };

    explicit ControlTask(Config config);

    /// Worker shutdown before referenced dependencies are destroyed
    ~ControlTask() override { stop(); }

protected:
    /// Coherent read -> calculation -> complete publication
    void execute() override;

    /// Stop lifecycle log
    void onStop() override;

private:
    sc::application::YawLut yawLut_;
    int numTurbines_;
};
