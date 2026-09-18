#include "catch2/catch_test_macros.hpp"
#include "sc/TurbineParameters.hpp"
#include "sc/application/Monitoring.hpp"

#include <algorithm>
#include <stdexcept>

namespace {

sc::application::MonitoringInput monitoringInput(std::size_t turbineCount, uint64_t currentTimeMs) {
    sc::application::MonitoringInput input;
    input.currentTimeMs = currentTimeMs;
    input.connectedTurbines = static_cast<int>(turbineCount);
    input.turbines.resize(turbineCount);
    return input;
}

const sc::application::AlarmEvidence* findEvidence(const sc::application::MonitoringResult& result,
                                                    sc::application::AlarmType alarm) {
    const auto match = std::find_if(result.evidence.begin(), result.evidence.end(),
        [alarm](const sc::application::AlarmEvidence& evidence) { return evidence.alarm == alarm; });
    return match == result.evidence.end() ? nullptr : &*match;
}

} // namespace

TEST_CASE("monitoring rejects a mismatched turbine count") {
    sc::application::MonitoringDetectors detectors(2);
    REQUIRE_THROWS_AS(detectors.evaluate(monitoringInput(1, 1000)), std::invalid_argument);
}

TEST_CASE("static telemetry bounds report the turbine and offending value") {
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

TEST_CASE("orientation detector retains its estimate between cycles") {
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

TEST_CASE("power tracking requires a persistent mismatch") {
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

TEST_CASE("telemetry freeze requires both a full window and persistence") {
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

TEST_CASE("wind direction comparison follows the shortest angular path") {
    sc::application::MonitoringDetectors detectors(1);
    auto input = monitoringInput(1, 1000);
    input.farmWindDirection = 359.0F;
    input.turbines[0].windDirection = 1.0;

    REQUIRE_FALSE(detectors.evaluateDetector(sc::application::AlarmType::WindDirection, input)
                      .isActive(sc::application::AlarmType::WindDirection));
}

TEST_CASE("acknowledged alarms clear only after their condition clears") {
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

TEST_CASE("inactive latched alarms remain visible until acknowledged") {
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
