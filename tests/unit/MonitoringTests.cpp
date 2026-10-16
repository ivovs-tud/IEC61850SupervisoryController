#include "catch2/catch_test_macros.hpp"
#include "sc/application/Monitoring.hpp"
#include "sc/model/TurbineParameters.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {

sc::application::MonitoringInput monitoringInput(std::size_t turbineCount, uint64_t currentTimeMs)
{
    sc::application::MonitoringInput input;
    input.currentTimeMs = currentTimeMs;
    input.connectedTurbines = static_cast<int>(turbineCount);
    input.turbines.resize(turbineCount);
    return input;
}

const sc::application::AlarmEvidence* findEvidence(const sc::application::MonitoringResult& result, sc::application::AlarmType alarm)
{
    const auto match = std::find_if(result.evidence.begin(), result.evidence.end(),
                                    [alarm](const sc::application::AlarmEvidence& evidence) { return evidence.alarm == alarm; });
    return match == result.evidence.end() ? nullptr : &*match;
}

} // namespace

TEST_CASE("monitoring rejects a mismatched turbine count")
{
    sc::application::MonitoringDetectors detectors(2);
    REQUIRE_THROWS_AS(detectors.evaluate(monitoringInput(1, 1000)), std::invalid_argument);
}

TEST_CASE("monitoring rejects an invalid connected turbine count")
{
    sc::application::MonitoringDetectors detectors(1);
    auto input = monitoringInput(1, 1000);
    input.connectedTurbines = -1;
    REQUIRE_THROWS_AS(detectors.evaluate(input), std::invalid_argument);

    input.connectedTurbines = 2;
    REQUIRE_THROWS_AS(detectors.evaluate(input), std::invalid_argument);
}

TEST_CASE("static telemetry bounds report the turbine and offending value")
{
    sc::application::MonitoringDetectors detectors(1);
    auto input = monitoringInput(1, 1000);
    input.turbines[0].windSpeed = 50.0;
    input.turbines[0].windSpeedTimeMs = 1000;

    const auto result = detectors.evaluateDetector(sc::application::AlarmType::StaticTelemetryBounds, input);
    REQUIRE(result.isActive(sc::application::AlarmType::StaticTelemetryBounds));
    const auto* evidence = findEvidence(result, sc::application::AlarmType::StaticTelemetryBounds);
    REQUIRE(evidence != nullptr);
    REQUIRE(evidence->turbineId == 1);
    REQUIRE(evidence->measured == 50.0);
    REQUIRE(evidence->expected == 1.5 * sc::TurbineParameters::cutOutWindSpeed);
    REQUIRE(evidence->timestampMs == 1000);
}

TEST_CASE("orientation detector retains its estimate between cycles")
{
    sc::application::MonitoringDetectors detectors(1);
    auto input = monitoringInput(1, 1000);
    input.turbines[0].yaw = 0.0;
    input.turbines[0].yawTimeMs = 1000;
    input.turbines[0].yawSetpoint = 90.0F;
    REQUIRE_FALSE(detectors.evaluateDetector(sc::application::AlarmType::OrientationMisalignment, input)
                      .isActive(sc::application::AlarmType::OrientationMisalignment));

    input.currentTimeMs = 2000;
    input.turbines[0].yaw = 30.0;
    input.turbines[0].yawTimeMs = 2000;
    const auto result = detectors.evaluateDetector(sc::application::AlarmType::OrientationMisalignment, input);

    REQUIRE(result.isActive(sc::application::AlarmType::OrientationMisalignment));
    const auto* evidence = findEvidence(result, sc::application::AlarmType::OrientationMisalignment);
    REQUIRE(evidence != nullptr);
    REQUIRE(evidence->turbineId == 1);
    REQUIRE(evidence->measured == 30.0);
    REQUIRE(evidence->expected == 5.0);
    REQUIRE(evidence->threshold == 8.0);
}

TEST_CASE("power tracking requires a persistent mismatch")
{
    sc::application::MonitoringDetectors detectors(1);
    auto input = monitoringInput(1, 1000);
    input.farmWindSpeed = 10.0F;
    input.turbines[0].powerTimeMs = 1000;
    input.turbines[0].powerHistory = {0.0};
    REQUIRE_FALSE(detectors.evaluateDetector(sc::application::AlarmType::MeasuredPowerVsExpected, input)
                      .isActive(sc::application::AlarmType::MeasuredPowerVsExpected));

    input.currentTimeMs = 6000;
    input.turbines[0].powerTimeMs = 6000;
    const auto result = detectors.evaluateDetector(sc::application::AlarmType::MeasuredPowerVsExpected, input);
    REQUIRE(result.isActive(sc::application::AlarmType::MeasuredPowerVsExpected));
    REQUIRE(findEvidence(result, sc::application::AlarmType::MeasuredPowerVsExpected) != nullptr);
}

TEST_CASE("power expectation uses filtered turbine speed and global wind direction")
{
    sc::application::MonitoringDetectors detectors(1);
    auto input = monitoringInput(1, 1000);
    input.farmWindSpeed = 5.0F;
    input.farmWindDirection = 0.0F;
    auto& turbine = input.turbines[0];
    turbine.windSpeed = 20.0;
    turbine.windSpeedTimeMs = 1000;
    turbine.filteredWindSpeed = 10.0;
    turbine.filteredWindSpeedTimeMs = 1000;
    turbine.windDirection = 180.0;
    turbine.windDirectionTimeMs = 1000;
    turbine.yaw = 0.0;
    turbine.yawTimeMs = 1000;
    turbine.power = 0.0;
    turbine.powerTimeMs = 1000;
    turbine.powerHistory = {0.0};
    detectors.evaluateDetector(sc::application::AlarmType::MeasuredPowerVsExpected, input);

    input.currentTimeMs = 6000;
    turbine.filteredWindSpeedTimeMs = 6000;
    turbine.yawTimeMs = 6000;
    turbine.powerTimeMs = 6000;
    const auto result = detectors.evaluateDetector(sc::application::AlarmType::MeasuredPowerVsExpected, input);

    REQUIRE(result.isActive(sc::application::AlarmType::MeasuredPowerVsExpected));
    const auto* evidence = findEvidence(result, sc::application::AlarmType::MeasuredPowerVsExpected);
    REQUIRE(evidence != nullptr);
    REQUIRE(evidence->expected > 0.0);
}

TEST_CASE("telemetry freeze requires both a full window and persistence")
{
    sc::application::MonitoringDetectors detectors(1);
    auto input = monitoringInput(1, 1000);
    input.turbines[0].windSpeed = 8.0;
    input.turbines[0].windSpeedTimeMs = 1000;
    input.turbines[0].windSpeedHistory.assign(8, 8.0);
    REQUIRE_FALSE(detectors.evaluateDetector(sc::application::AlarmType::TelemetryFreeze, input)
                      .isActive(sc::application::AlarmType::TelemetryFreeze));

    input.currentTimeMs = 2500;
    input.turbines[0].windSpeedTimeMs = 2500;
    const auto result = detectors.evaluateDetector(sc::application::AlarmType::TelemetryFreeze, input);
    REQUIRE(result.isActive(sc::application::AlarmType::TelemetryFreeze));
    REQUIRE(findEvidence(result, sc::application::AlarmType::TelemetryFreeze)->turbineId == 1);
}

TEST_CASE("wind direction comparison follows the shortest angular path")
{
    sc::application::MonitoringDetectors detectors(1);
    auto input = monitoringInput(1, 1000);
    input.farmWindDirection = 359.0F;
    input.turbines[0].windDirection = 1.0;
    input.turbines[0].windDirectionTimeMs = 1000;

    REQUIRE_FALSE(
        detectors.evaluateDetector(sc::application::AlarmType::WindDirection, input).isActive(sc::application::AlarmType::WindDirection));
}

TEST_CASE("measurement detectors ignore stale and future-dated values")
{
    sc::application::MonitoringDetectors detectors(1);
    auto input = monitoringInput(1, 30000);
    auto& turbine = input.turbines[0];
    turbine.power = 1.0e7;
    turbine.rotorSpeed = 0.0;
    turbine.generatorTorque = 0.0;
    turbine.powerTimeMs = 1000;
    turbine.rotorSpeedTimeMs = 1000;
    turbine.generatorTorqueTimeMs = 1000;
    REQUIRE_FALSE(detectors.evaluateDetector(sc::application::AlarmType::PowerTorqueRotorSpeed, input)
                      .isActive(sc::application::AlarmType::PowerTorqueRotorSpeed));

    input.farmWindDirection = 180.0F;
    turbine.windDirection = 0.0;
    turbine.windDirectionTimeMs = 31000;
    REQUIRE_FALSE(
        detectors.evaluateDetector(sc::application::AlarmType::WindDirection, input).isActive(sc::application::AlarmType::WindDirection));
}

TEST_CASE("persistent detectors reset rather than alarming on stale telemetry")
{
    sc::application::MonitoringDetectors detectors(1);
    auto input = monitoringInput(1, 1000);
    auto& turbine = input.turbines[0];
    input.farmWindSpeed = 10.0F;
    turbine.power = 0.0;
    turbine.powerHistory = {0.0};
    turbine.powerTimeMs = 1000;
    REQUIRE_FALSE(detectors.evaluateDetector(sc::application::AlarmType::MeasuredPowerVsExpected, input)
                      .isActive(sc::application::AlarmType::MeasuredPowerVsExpected));

    input.currentTimeMs = 30000;
    REQUIRE_FALSE(detectors.evaluateDetector(sc::application::AlarmType::MeasuredPowerVsExpected, input)
                      .isActive(sc::application::AlarmType::MeasuredPowerVsExpected));

    input.currentTimeMs = 1000;
    turbine.rotorSpeed = 0.0;
    turbine.rotorSpeedTimeMs = 1000;
    turbine.generatorTorque = 0.0;
    turbine.generatorTorqueTimeMs = 1000;
    detectors.evaluateDetector(sc::application::AlarmType::DrivetrainUnderResponse, input);
    input.currentTimeMs = 30000;
    REQUIRE_FALSE(detectors.evaluateDetector(sc::application::AlarmType::DrivetrainUnderResponse, input)
                      .isActive(sc::application::AlarmType::DrivetrainUnderResponse));

    input.currentTimeMs = 30000;
    turbine.windSpeedHistory = {8.0, 8.0, 8.0, 8.0, 20.0};
    turbine.windSpeedTimeMs = 1;
    for (int strike = 0; strike < 3; ++strike) {
        REQUIRE_FALSE(detectors.evaluateDetector(sc::application::AlarmType::WindSpeedChange, input)
                          .isActive(sc::application::AlarmType::WindSpeedChange));
    }
}

TEST_CASE("static bounds report recent non-finite telemetry")
{
    sc::application::MonitoringDetectors detectors(1);
    auto input = monitoringInput(1, 1000);
    input.turbines[0].windSpeed = std::numeric_limits<double>::quiet_NaN();
    input.turbines[0].windSpeedTimeMs = 1000;

    REQUIRE(detectors.evaluateDetector(sc::application::AlarmType::StaticTelemetryBounds, input)
                .isActive(sc::application::AlarmType::StaticTelemetryBounds));
}

TEST_CASE("acknowledged alarms clear only after their condition clears")
{
    sc::application::AlarmStateTracker alarms;
    sc::application::MonitoringResult result;
    result.active[static_cast<std::size_t>(sc::application::AlarmType::StaticTelemetryBounds)] = true;

    alarms.update(result);
    REQUIRE(alarms.state(sc::application::AlarmType::StaticTelemetryBounds).active);
    REQUIRE(alarms.state(sc::application::AlarmType::StaticTelemetryBounds).latched);
    REQUIRE_FALSE(alarms.state(sc::application::AlarmType::StaticTelemetryBounds).acknowledged);

    alarms.acknowledge();
    REQUIRE(alarms.visible(sc::application::AlarmType::StaticTelemetryBounds));
    REQUIRE(alarms.state(sc::application::AlarmType::StaticTelemetryBounds).acknowledged);

    result.active.fill(false);
    alarms.update(result);
    REQUIRE_FALSE(alarms.visible(sc::application::AlarmType::StaticTelemetryBounds));
}

TEST_CASE("inactive latched alarms remain visible until acknowledged")
{
    sc::application::AlarmStateTracker alarms;
    sc::application::MonitoringResult result;
    result.active[static_cast<std::size_t>(sc::application::AlarmType::WindSpeedChange)] = true;
    alarms.update(result);

    result.active.fill(false);
    alarms.update(result);
    REQUIRE_FALSE(alarms.state(sc::application::AlarmType::WindSpeedChange).active);
    REQUIRE(alarms.visible(sc::application::AlarmType::WindSpeedChange));

    alarms.acknowledge();
    REQUIRE_FALSE(alarms.visible(sc::application::AlarmType::WindSpeedChange));
}
