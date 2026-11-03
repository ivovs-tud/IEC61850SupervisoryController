#pragma once

#include "sc/application/PowerSharingMode.hpp"
#include "sc/application/YawLut.hpp"
#include "sc/runtime/PeriodicTask.hpp"

#include <chrono>

/// Periodic adapter for the pure control calculation
class ControlTask : public PeriodicTask {
    public:
    /// Startup dependencies and schedule
    struct Config {
        int numTurbines;
        std::chrono::milliseconds period{std::chrono::milliseconds(10)};
        sc::application::PowerSharingMode powerSharingMode{sc::application::PowerSharingMode::EQUAL};
    };

    ControlTask(Config config, sc::application::YawLut yawLut);

    /// Worker shutdown before referenced dependencies are destroyed
    ~ControlTask() override
    {
        stop();
    }

    protected:
    /// Coherent read -> calculation -> complete publication
    void execute() override;

    /// Stop lifecycle log
    void onStop() override;

    private:
    sc::application::YawLut yawLut_;
    int numTurbines_;
    sc::application::PowerSharingMode powerSharingMode_;
};
