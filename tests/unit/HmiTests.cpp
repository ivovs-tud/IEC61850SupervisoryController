#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "sc/communication/hmi/HmiConfig.hpp"

TEST_CASE("HMI data collection reads each shared-data section") {
    SharedData shared;
    shared.configureTurbineCount(2);
    {
        std::lock_guard<std::mutex> lock(shared.collected.mutex);
        shared.collected.lastPower = {1.0, 2.0};
        shared.collected.lastWS = {7.0, 8.0};
    }
    {
        std::lock_guard<std::mutex> lock(shared.processed.mutex);
        shared.processed.connectedTurbines = 2;
        shared.processed.windSpeed = 7.5F;
        shared.processed.measuredTotalPowerHistory.push_back(3.0);
    }
    {
        std::lock_guard<std::mutex> lock(shared.control.mutex);
        shared.control.powerSetpoints = {4.0F, -1.0F};
        shared.control.turbineController = {ControlData::controllerDownregulation,
                                            ControlData::controllerDownregulation};
    }
    {
        std::lock_guard<std::mutex> lock(shared.monitoring.mutex);
        shared.monitoring.alarmStaticBounds = true;
    }
    {
        std::lock_guard<std::mutex> lock(shared.interface.mutex);
        shared.interface.systemRunning = true;
        shared.interface.attackTapEnabled = 3;
    }

    const HmiData data = collectHmiData(shared);

    REQUIRE(data.turbinePower == std::vector<double>{1.0, 2.0});
    REQUIRE(data.turbineWindSpeed == std::vector<double>{7.0, 8.0});
    REQUIRE(data.powerSetpoints == std::vector<float>{4.0F, -1.0F});
    REQUIRE(data.measuredTotalPower == 3.0);
    REQUIRE(data.farmWindSpeed == 7.5);
    REQUIRE(data.connectedTurbines == 2);
    REQUIRE(data.operationMode == static_cast<int>(ControlData::controllerDownregulation));
    REQUIRE(data.systemRunning);
    REQUIRE(data.alarmStaticBounds);
    REQUIRE(data.attackTapEnabled == 3);
}

TEST_CASE("HMI signal accessors use the collected display data") {
    const HmiConfig config = defaultHmiConfig(2);
    HmiData data;
    data.turbinePower = {1.0, 2.0};
    data.powerSetpoints = {3.0F, -1.0F};

    const std::vector<double> values = config.signals.front().accessor(data);

    REQUIRE(values.size() == 4);
    REQUIRE(values[0] == 1.0);
    REQUIRE(values[1] == 3.0);
    REQUIRE(values[2] == 2.0);
    REQUIRE(std::isnan(values[3]));
}
