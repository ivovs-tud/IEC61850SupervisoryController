#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "sc/application/SignalProcessing.hpp"
#include "sc/model/TurbineParameters.hpp"

#include <limits>
#include <stdexcept>

namespace {

sc::application::TurbineSignalInput turbine(double windSpeed,
                                             double windDirection,
                                             uint64_t timestampMs) {
    sc::application::TurbineSignalInput input;
    input.windSpeed = windSpeed;
    input.windSpeedTimeMs = timestampMs;
    input.windDirection = windDirection;
    input.windDirectionTimeMs = timestampMs;
    input.receivedPower = windSpeed * 10.0;
    input.measuredPower = windSpeed * 20.0;
    input.powerTimeMs = timestampMs;
    input.newestMeasurementTimeMs = timestampMs;
    return input;
}

sc::application::SignalProcessingConfig immediateUpdates() {
    sc::application::SignalProcessingConfig config;
    config.windSpeedUpdateWeight = 1.0F;
    config.windDirectionUpdateWeight = 1.0F;
    return config;
}

} // namespace

TEST_CASE("signal processing excludes stale wind data") {
    sc::application::SignalProcessingInput input;
    input.currentTimeMs = 4001;
    input.previousWindSpeed = 7.0F;
    input.previousWindDirection = 42.0F;
    input.turbines.push_back(turbine(12.0, 180.0, 2000));

    const auto result = sc::application::processSignals(input, immediateUpdates());

    REQUIRE(result.connectedTurbines == 0);
    REQUIRE(result.availablePower == std::vector<double>{0.0});
    REQUIRE(result.windSpeed == Catch::Approx(7.0F));
    REQUIRE(result.windDirection == Catch::Approx(42.0F));
    REQUIRE(result.totalReceivedPower == Catch::Approx(0.0));
    REQUIRE(result.totalMeasuredPower == Catch::Approx(0.0));
}

TEST_CASE("signal processing averages fewer than three fresh turbines") {
    sc::application::SignalProcessingInput input;
    input.currentTimeMs = 1000;
    input.turbines.push_back(turbine(8.0, 90.0, 1000));
    input.turbines.push_back(turbine(10.0, 90.0, 1000));

    const auto result = sc::application::processSignals(input, immediateUpdates());

    REQUIRE(result.connectedTurbines == 2);
    REQUIRE(result.windSpeed == Catch::Approx(9.0F));
}

TEST_CASE("signal processing maintains filtered wind speed for each turbine") {
    auto input = sc::application::SignalProcessingInput{};
    input.currentTimeMs = 1000;
    input.previousFilteredWindSpeeds = {6.0};
    input.previousFilteredWindSpeedTimeMs = {900};
    input.turbines.push_back(turbine(10.0, 90.0, 1000));
    auto config = immediateUpdates();
    config.windSpeedUpdateWeight = 0.5F;

    const auto result = sc::application::processSignals(input, config);

    REQUIRE(result.filteredWindSpeeds == std::vector<double>{8.0});
    REQUIRE(result.filteredWindSpeedTimeMs == std::vector<uint64_t>{1000});
    REQUIRE(result.availablePower[0] == Catch::Approx(sc::application::calculateAvailablePower(8.0)));
}

TEST_CASE("signal processing handles wind direction wraparound") {
    auto config = immediateUpdates();

    sc::application::SignalProcessingInput meanInput;
    meanInput.currentTimeMs = 1000;
    meanInput.turbines.push_back(turbine(8.0, 359.0, 1000));
    meanInput.turbines.push_back(turbine(8.0, 1.0, 1000));
    const auto meanResult = sc::application::processSignals(meanInput, config);
    REQUIRE(meanResult.windDirection == Catch::Approx(0.0F).margin(0.001F));

    config.windDirectionUpdateWeight = 0.5F;
    sc::application::SignalProcessingInput smoothingInput;
    smoothingInput.currentTimeMs = 1000;
    smoothingInput.previousWindDirection = 359.0F;
    smoothingInput.turbines.push_back(turbine(8.0, 1.0, 1000));
    const auto smoothingResult = sc::application::processSignals(smoothingInput, config);
    REQUIRE(smoothingResult.windDirection == Catch::Approx(0.0F).margin(0.001F));
}

TEST_CASE("available power respects turbine operating bounds") {
    REQUIRE(sc::application::calculateAvailablePower(sc::TurbineParameters::cutInWindSpeed - 0.1) == 0.0);
    REQUIRE(sc::application::calculateAvailablePower(sc::TurbineParameters::cutInWindSpeed) > 0.0);
    REQUIRE(sc::application::calculateAvailablePower(24.0) == Catch::Approx(sc::TurbineParameters::ratedPower));
    REQUIRE(sc::application::calculateAvailablePower(sc::TurbineParameters::cutOutWindSpeed) == 0.0);
    REQUIRE(sc::application::calculateAvailablePower(100.0) == 0.0);
}

TEST_CASE("signal processing preserves filter state without turbines") {
    sc::application::SignalProcessingInput input;
    input.currentTimeMs = 1000;
    input.previousWindSpeed = 6.0F;
    input.previousWindDirection = 270.0F;

    const auto result = sc::application::processSignals(input);

    REQUIRE(result.connectedTurbines == 0);
    REQUIRE(result.availablePower.empty());
    REQUIRE(result.windSpeed == Catch::Approx(6.0F));
    REQUIRE(result.windDirection == Catch::Approx(270.0F));
}

TEST_CASE("signal processing excludes non-finite telemetry and validates filter inputs") {
    auto input = sc::application::SignalProcessingInput{};
    input.currentTimeMs = 1000;
    input.previousWindSpeed = 6.0F;
    input.previousWindDirection = 270.0F;
    input.turbines.push_back(turbine(
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(), 1000));

    const auto result = sc::application::processSignals(input, immediateUpdates());
    REQUIRE(result.availablePower == std::vector<double>{0.0});
    REQUIRE(result.windSpeed == Catch::Approx(6.0F));
    REQUIRE(result.windDirection == Catch::Approx(270.0F));
    REQUIRE(result.totalReceivedPower == Catch::Approx(0.0));
    REQUIRE(result.totalMeasuredPower == Catch::Approx(0.0));

    auto config = immediateUpdates();
    config.windSpeedUpdateWeight = 1.5F;
    REQUIRE_THROWS_AS(sc::application::processSignals(input, config), std::invalid_argument);
}
