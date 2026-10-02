#include "sc/tasks/ControlTask.hpp"
#include "sc/tasks/SignalProcessingTask.hpp"

#include "sc/model/SharedData.hpp"
#include "sc/runtime/Time.hpp"
#include "support/TemporaryCsv.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

class TestControlTask : public ControlTask {
public:
    using ControlTask::ControlTask;
    void runOnce() { execute(); }
};

class TestSignalProcessingTask : public SignalProcessingTask {
public:
    using SignalProcessingTask::SignalProcessingTask;
    void runOnce() { execute(); }
};

sc::application::YawLut testYawLut() {
    const sc::test::TemporaryCsv file(
        "ws,wd,WT1,WT2\n"
        "0,0,0.5,-0.5\n"
        "0,20,0.5,-0.5\n"
        "10,0,0.5,-0.5\n"
        "10,20,0.5,-0.5\n");
    return sc::application::YawLut(file.path());
}

} // namespace

TEST_CASE("control task publishes one complete setpoint update") {
    auto& data = SharedData::instance();
    data.configureTurbineCount(2);
    {
        std::lock_guard<std::mutex> lock(data.control.mutex);
        data.control.requestedPower = 100.0F;
        data.control.yawSteeringEnabled = true;
    }
    {
        std::lock_guard<std::mutex> lock(data.processed.mutex);
        data.processed.windSpeed = 5.0F;
        data.processed.windDirection = 0.0F;
    }

    TestControlTask task({2, std::chrono::milliseconds(10)}, testYawLut());
    task.runOnce();

    std::lock_guard<std::mutex> lock(data.control.mutex);
    REQUIRE(data.control.powerSetpoints == std::vector<float>{50.0F, 50.0F});
    REQUIRE(data.control.yawSetpoints == std::vector<float>{359.0F, 1.0F});
}

TEST_CASE("control task leaves both outputs unchanged when publication cannot complete") {
    auto& data = SharedData::instance();
    data.configureTurbineCount(3);
    {
        std::lock_guard<std::mutex> lock(data.control.mutex);
        data.control.requestedPower = 100.0F;
        data.control.yawSteeringEnabled = true;
        data.control.powerSetpoints = {11.0F, 12.0F, 13.0F};
        data.control.yawSetpoints = {21.0F, 22.0F, 23.0F};
    }
    {
        std::lock_guard<std::mutex> lock(data.processed.mutex);
        data.processed.windSpeed = 5.0F;
        data.processed.windDirection = 0.0F;
    }

    TestControlTask task({2, std::chrono::milliseconds(10)}, testYawLut());
    REQUIRE_THROWS_AS(task.runOnce(), std::logic_error);

    std::lock_guard<std::mutex> lock(data.control.mutex);
    REQUIRE(data.control.powerSetpoints == std::vector<float>{11.0F, 12.0F, 13.0F});
    REQUIRE(data.control.yawSetpoints == std::vector<float>{21.0F, 22.0F, 23.0F});
}

TEST_CASE("control task uses the published available-power estimates in available-power mode") {
    auto& data = SharedData::instance();
    data.configureTurbineCount(2);
    {
        std::lock_guard<std::mutex> lock(data.control.mutex);
        data.control.requestedPower = 100.0F;
        data.control.yawSteeringEnabled = false;
    }
    {
        std::lock_guard<std::mutex> lock(data.processed.mutex);
        data.processed.windSpeed = 5.0F;
        data.processed.windDirection = 0.0F;
        data.processed.availablePower = {20.0, 80.0};
    }

    TestControlTask task(
        {2, std::chrono::milliseconds(10), sc::application::PowerSharingMode::AVAILABLE_POWER},
        testYawLut());
    task.runOnce();

    std::lock_guard<std::mutex> lock(data.control.mutex);
    REQUIRE(data.control.powerSetpoints == std::vector<float>{20.0F, 80.0F});
}

TEST_CASE("signal-processing task publishes finite farm data from fresh measurements") {
    auto& data = SharedData::instance();
    data.configureTurbineCount(2);
    const uint64_t now = getCurrentTimeMs();
    {
        std::lock_guard<std::mutex> lock(data.collected.mutex);
        data.collected.lastWS = {8.0, std::numeric_limits<double>::quiet_NaN()};
        data.collected.lastWD = {270.0, 90.0};
        data.collected.lastPower = {10.0, 20.0};
        data.collected.measuredPower = {11.0, 21.0};
        data.collected.lastWS_t = {now, 0};
        data.collected.lastWD_t = {now, 0};
        data.collected.lastPower_t = {now, 0};
        data.collected.lastYawOffset_t = {now, 0};
        data.collected.lastRPM_t = {now, 0};
        data.collected.lastGenTorque_t = {now, 0};
    }
    {
        std::lock_guard<std::mutex> lock(data.processed.mutex);
        data.processed.connectedTurbines = 0;
        data.processed.totalReceivedPower = 0.0;
        data.processed.windSpeed = 4.0F;
        data.processed.windDirection = 270.0F;
        data.processed.filteredWindSpeeds = {0.0, 0.0};
        data.processed.filteredWindSpeedTimeMs = {0, 0};
        data.processed.measuredTotalPowerHistory.clear();
    }

    TestSignalProcessingTask task(std::chrono::milliseconds(10));
    task.runOnce();

    std::lock_guard<std::mutex> lock(data.processed.mutex);
    REQUIRE(data.processed.connectedTurbines == 1);
    REQUIRE(data.processed.totalReceivedPower == Catch::Approx(10.0));
    REQUIRE(data.processed.measuredTotalPowerHistory.back() == Catch::Approx(11.0));
    REQUIRE(data.processed.windSpeed == Catch::Approx(4.4F));
    REQUIRE(data.processed.windDirection == Catch::Approx(270.0F));
    REQUIRE(data.processed.availablePower[0] > 0.0);
    REQUIRE(data.processed.availablePower[1] == 0.0);
}
