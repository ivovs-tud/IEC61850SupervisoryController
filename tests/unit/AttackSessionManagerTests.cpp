#include "sc/communication/attack/AttackSessionManager.hpp"
#include "support/FakeClock.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <vector>

using AttackInterface::SignalType;
using sc::application::AttackSessionManager;

TEST_CASE("attack session state validates its configuration")
{
    FakeClock clock;

    REQUIRE_THROWS_AS(AttackSessionManager(0, {SignalType::WIND_SPEED}, clock), std::invalid_argument);
    REQUIRE_THROWS_AS(AttackSessionManager(1, {}, clock), std::invalid_argument);
    REQUIRE_THROWS_AS(AttackSessionManager(1, {SignalType::WIND_SPEED}, clock, std::chrono::milliseconds(0)), std::invalid_argument);

    AttackSessionManager state(1, {SignalType::WIND_SPEED}, clock);
    REQUIRE_FALSE(state.startSession(""));
    REQUIRE_FALSE(state.startSession("bad\nlabel"));
}

TEST_CASE("one attack client owns the tap and FDI state")
{
    FakeClock clock;
    AttackSessionManager state(2, {SignalType::WIND_SPEED, SignalType::YAW_ANGLE}, clock);

    const auto started = state.startSession("team / scenario");
    REQUIRE(started);
    REQUIRE_FALSE(state.startSession("other client"));

    REQUIRE(state.setTapEnabled(1, SignalType::YAW_ANGLE, true));
    REQUIRE(state.setFdiEnabled(2, SignalType::WIND_SPEED, true));
    REQUIRE(state.tapEnabled(1, SignalType::YAW_ANGLE));
    REQUIRE(state.fdiEnabled(2, SignalType::WIND_SPEED));
    REQUIRE_FALSE(state.fdiValue(2, SignalType::WIND_SPEED));
    REQUIRE(state.setFdiValue(2, SignalType::WIND_SPEED, 42.0F));
    REQUIRE(state.fdiValue(2, SignalType::WIND_SPEED) == 42.0F);
    REQUIRE(state.setFdiEnabled(2, SignalType::WIND_SPEED, false));
    REQUIRE_FALSE(state.fdiValue(2, SignalType::WIND_SPEED));
    REQUIRE_FALSE(state.setTapEnabled(3, SignalType::YAW_ANGLE, true));
    REQUIRE_FALSE(state.setFdiEnabled(1, SignalType::NONE, true));
}

TEST_CASE("ending an attack session resets every link")
{
    FakeClock clock;
    AttackSessionManager state(2, {SignalType::WIND_SPEED, SignalType::YAW_ANGLE}, clock);
    REQUIRE(state.startSession("temporary attack"));
    REQUIRE(state.setTapEnabled(1, SignalType::YAW_ANGLE, true));
    REQUIRE(state.setFdiEnabled(2, SignalType::WIND_SPEED, true));

    const auto closed = state.endSession("client release");
    REQUIRE(closed);
    REQUIRE(closed->label == "temporary attack");
    REQUIRE(closed->reason == "client release");
    REQUIRE_FALSE(state.tapEnabled(1, SignalType::YAW_ANGLE));
    REQUIRE_FALSE(state.fdiEnabled(2, SignalType::WIND_SPEED));
    REQUIRE_FALSE(state.endSession("duplicate release"));
}

TEST_CASE("heartbeats extend the attack client lease")
{
    FakeClock clock;
    AttackSessionManager state(1, {SignalType::WIND_SPEED}, clock, std::chrono::milliseconds(750));
    REQUIRE(state.startSession("lease test"));

    clock.advance(std::chrono::milliseconds(500));
    REQUIRE(state.heartbeat());
    clock.advance(std::chrono::milliseconds(500));
    REQUIRE_FALSE(state.expireSession());
    clock.advance(std::chrono::milliseconds(250));

    const auto expired = state.expireSession();
    REQUIRE(expired);
    REQUIRE(expired->reason == "lease timeout");
    REQUIRE_FALSE(state.heartbeat());
}

TEST_CASE("a new session cannot revive controls or values from an ended session")
{
    FakeClock clock;
    AttackSessionManager state(1, {SignalType::YAW_SETPOINT}, clock);

    const auto first = state.startSession("first session");
    REQUIRE(first);
    REQUIRE(state.setTapEnabled(1, SignalType::YAW_SETPOINT, true));
    REQUIRE(state.setFdiEnabled(1, SignalType::YAW_SETPOINT, true));
    REQUIRE(state.setFdiValue(1, SignalType::YAW_SETPOINT, 91.0F));
    REQUIRE(state.endSession("connection lost"));
    REQUIRE_FALSE(state.heartbeat());

    const auto second = state.startSession("second session");
    REQUIRE(second);
    REQUIRE(second.id != first.id);
    REQUIRE_FALSE(state.tapEnabled(1, SignalType::YAW_SETPOINT));
    REQUIRE_FALSE(state.fdiEnabled(1, SignalType::YAW_SETPOINT));
    REQUIRE_FALSE(state.fdiValue(1, SignalType::YAW_SETPOINT));
}
