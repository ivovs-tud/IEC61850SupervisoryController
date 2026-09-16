#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "sc/application/ControlCalculation.hpp"
#include "support/TemporaryCsv.hpp"

namespace {

sc::application::YawLut testYawLut() {
    const sc::test::TemporaryCsv file(
        "ws,wd,WT1,WT2\n"
        "0,0,0.5,-0.5\n"
        "0,20,0.5,-0.5\n"
        "10,0,0.5,-0.5\n"
        "10,20,0.5,-0.5\n");
    return sc::application::YawLut(file.path());
}

sc::application::ControlInputs defaultInputs() {
    sc::application::ControlInputs inputs;
    inputs.turbineCount = 2;
    inputs.requestedReferencePower = 100.0F;
    inputs.windSpeed = 5.0F;
    inputs.windDirection = 0.0F;
    inputs.yawSteeringEnabled = false;
    return inputs;
}

} // namespace

TEST_CASE("control calculation divides requested power equally") {
    const auto lut = testYawLut();
    const auto setpoints = sc::application::calculateControlSetpoints(defaultInputs(), lut);

    REQUIRE(setpoints.turbinePower.size() == 2);
    REQUIRE(setpoints.turbinePower[0] == Catch::Approx(50.0F));
    REQUIRE(setpoints.turbinePower[1] == Catch::Approx(50.0F));
}

TEST_CASE("control calculation broadcasts negative reference power to every turbine") {
    const auto lut = testYawLut();
    auto inputs = defaultInputs();
    inputs.requestedReferencePower = -1.0F;

    const auto sentinelSetpoints = sc::application::calculateControlSetpoints(inputs, lut);

    REQUIRE(sentinelSetpoints.turbinePower.size() == 2);
    REQUIRE(sentinelSetpoints.turbinePower[0] == Catch::Approx(-1.0F));
    REQUIRE(sentinelSetpoints.turbinePower[1] == Catch::Approx(-1.0F));

    inputs.requestedReferencePower = -300.0F;
    const auto negativeSetpoints = sc::application::calculateControlSetpoints(inputs, lut);
    REQUIRE(negativeSetpoints.turbinePower[0] == Catch::Approx(-300.0F));
    REQUIRE(negativeSetpoints.turbinePower[1] == Catch::Approx(-300.0F));
}

TEST_CASE("control calculation rounds wind direction when yaw steering is disabled") {
    const auto lut = testYawLut();
    auto inputs = defaultInputs();
    inputs.windDirection = 12.9F;

    const auto setpoints = sc::application::calculateControlSetpoints(inputs, lut);

    REQUIRE(setpoints.turbineYaw.size() == 2);
    REQUIRE(setpoints.turbineYaw[0] == 13);
    REQUIRE(setpoints.turbineYaw[1] == 13);
}

TEST_CASE("control calculation applies LUT offsets with nearest-integer rounding") {
    const auto lut = testYawLut();
    auto inputs = defaultInputs();
    inputs.yawSteeringEnabled = true;

    const auto setpoints = sc::application::calculateControlSetpoints(inputs, lut);

    REQUIRE(setpoints.turbineYaw.size() == 2);
    REQUIRE(setpoints.turbineYaw[0] == 359);
    REQUIRE(setpoints.turbineYaw[1] == 1);
}

TEST_CASE("control calculation normalizes rounded orientation to one rotation") {
    const auto lut = testYawLut();
    auto inputs = defaultInputs();

    inputs.windDirection = 359.6F;
    auto setpoints = sc::application::calculateControlSetpoints(inputs, lut);
    REQUIRE(setpoints.turbineYaw[0] == 0);
    REQUIRE(setpoints.turbineYaw[1] == 0);

    inputs.windDirection = -0.6F;
    setpoints = sc::application::calculateControlSetpoints(inputs, lut);
    REQUIRE(setpoints.turbineYaw[0] == 359);
    REQUIRE(setpoints.turbineYaw[1] == 359);
}

TEST_CASE("control calculation preserves LUT and turbine count mismatch") {
    const auto lut = testYawLut();
    auto inputs = defaultInputs();
    inputs.turbineCount = 3;
    inputs.yawSteeringEnabled = true;

    const auto setpoints = sc::application::calculateControlSetpoints(inputs, lut);

    REQUIRE(setpoints.turbinePower.size() == 3);
    REQUIRE(setpoints.turbineYaw.size() == 2);
}

TEST_CASE("control calculation returns no setpoints for zero turbines") {
    const auto lut = testYawLut();
    auto inputs = defaultInputs();
    inputs.turbineCount = 0;
    inputs.yawSteeringEnabled = true;

    const auto setpoints = sc::application::calculateControlSetpoints(inputs, lut);

    REQUIRE(setpoints.turbinePower.empty());
    REQUIRE(setpoints.turbineYaw.empty());
}
