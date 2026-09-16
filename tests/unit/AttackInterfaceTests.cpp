#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "communication/AttackInterface.hpp"
#include "support/FakeAttackChannel.hpp"
#include "support/FakeClock.hpp"

namespace {

template <typename Message>
std::vector<uint8_t> asBytes(const Message& message) {
    std::vector<uint8_t> bytes(sizeof(Message));
    std::memcpy(bytes.data(), &message, sizeof(Message));
    return bytes;
}

template <typename Value>
Value readValue(const std::vector<uint8_t>& bytes, std::size_t offset) {
    Value value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(Value));
    return value;
}

std::vector<uint8_t> controlMessage(AttackInterface::ControlSignal signal,
                                    AttackInterface::TxDataType dataType,
                                    const std::vector<uint8_t>& enabled) {
    constexpr std::size_t prefixSize = 12;
    std::vector<uint8_t> bytes(prefixSize + enabled.size(), 0);
    bytes[0] = AttackInterface::CT_DATA;
    std::memcpy(bytes.data() + 4, &signal, sizeof(signal));
    std::memcpy(bytes.data() + 8, &dataType, sizeof(dataType));
    std::memcpy(bytes.data() + prefixSize, enabled.data(), enabled.size());
    return bytes;
}

} // namespace

TEST_CASE("attack interface handles configuration and simulation control") {
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(2, channel, clock);

    std::string configuredTeam;
    int configuredScenario = -1;
    int configuredController = -1;
    bool simulationStarted = false;

    attack.setCfgCommandCallback([&](const AttackInterface::CfgDataMessage& message) {
        configuredTeam = message.teamName;
        configuredScenario = message.scenarioId;
        configuredController = message.turbineController;
    });
    attack.setSimCtrlCommandCallback([&](const AttackInterface::SimCtrlMessage& message) {
        simulationStarted = message.simStart;
    });

    AttackInterface::CfgDataMessage config{};
    std::strncpy(config.teamName, "integration-team", sizeof(config.teamName) - 1);
    config.scenarioId = 7;
    config.turbineController = 2;
    channel.receive(asBytes(config));

    REQUIRE(configuredTeam == "integration-team");
    REQUIRE(configuredScenario == 7);
    REQUIRE(configuredController == 2);

    AttackInterface::SimCtrlMessage simulation{};
    simulation.simStart = true;
    channel.receive(asBytes(simulation));

    REQUIRE(simulationStarted);
    REQUIRE(channel.sentMessages().size() == 1);
    REQUIRE(channel.sentMessages()[0][0] == AttackInterface::SIM_CTRL);
    REQUIRE(channel.sentMessages()[0][1] == 1);
}

TEST_CASE("tap control gates outgoing observations per turbine") {
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(2, channel, clock);

    channel.receive(controlMessage(
        AttackInterface::CTRL_TAP,
        AttackInterface::TX_YAW,
        {1, 0}));

    float firstYaw = 12.5F;
    float secondYaw = 19.0F;
    attack.txData(1, AttackInterface::TX_YAW, &firstYaw);
    attack.txData(2, AttackInterface::TX_YAW, &secondYaw);

    REQUIRE(channel.sentMessages().size() == 1);
    const auto& message = channel.sentMessages().front();
    REQUIRE(message.size() == sizeof(AttackInterface::TxDataMessage));
    REQUIRE(message[0] == AttackInterface::TX_DATA);
    REQUIRE(message[1] == 1);
    REQUIRE(readValue<AttackInterface::TxDataType>(message, 4) == AttackInterface::TX_YAW);
    REQUIRE(readValue<float>(message, 12) == Catch::Approx(firstYaw));
}

TEST_CASE("FDI request accepts a matching response through the fake channel") {
    FakeAttackChannel channel;
    FakeClock clock(25'000);
    AttackInterface::AttackInterface attack(2, channel, clock);

    channel.receive(controlMessage(
        AttackInterface::CTRL_FDI,
        AttackInterface::TX_SPT_YAW,
        {1, 0}));

    channel.setSendObserver([&](const std::vector<uint8_t>& requestBytes) {
        if (requestBytes[0] != AttackInterface::RQ_DATA) {
            return;
        }

        AttackInterface::AtDataMessage response{};
        response.turbineId = requestBytes[1];
        response.dataType = readValue<AttackInterface::TxDataType>(requestBytes, 4);
        response.at_time = clock.unixTimeMilliseconds();
        response.fake_value = 91.25F;
        channel.receive(asBytes(response));
    });

    float value = 10.0F;
    REQUIRE(attack.overwrite(1, AttackInterface::TX_SPT_YAW, value) == AttackInterface::AI_OK);
    REQUIRE(value == Catch::Approx(91.25F));
    REQUIRE(channel.sentMessages().size() == 1);

    const auto& request = channel.sentMessages().front();
    REQUIRE(readValue<AttackInterface::TimeStamp>(request, 8) == 25'000);
    REQUIRE(readValue<AttackInterface::TimeStamp>(request, 16) == 25'250);
}

TEST_CASE("FDI timeout uses the injected clock and preserves the original value") {
    FakeAttackChannel channel;
    FakeClock clock(50'000);
    AttackInterface::AttackTiming timing;
    timing.requestLifetime = std::chrono::milliseconds(20);
    timing.responseTimeout = std::chrono::milliseconds(5);
    timing.responsePollPeriod = std::chrono::milliseconds(1);
    AttackInterface::AttackInterface attack(1, channel, clock, timing);

    channel.receive(controlMessage(
        AttackInterface::CTRL_FDI,
        AttackInterface::TX_PW,
        {1}));

    float value = 500.0F;
    REQUIRE(attack.overwrite(1, AttackInterface::TX_PW, value) == AttackInterface::AI_TIMEOUT);
    REQUIRE(value == Catch::Approx(500.0F));
    REQUIRE(clock.steadyNow().time_since_epoch() == std::chrono::milliseconds(5));
    REQUIRE(channel.sentMessages().size() == 1);

    const auto& request = channel.sentMessages().front();
    REQUIRE(readValue<AttackInterface::TimeStamp>(request, 8) == 50'000);
    REQUIRE(readValue<AttackInterface::TimeStamp>(request, 16) == 50'020);
}

TEST_CASE("configuration reset and malformed control messages leave tapping disabled") {
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(1, channel, clock);

    channel.receive(controlMessage(
        AttackInterface::CTRL_TAP,
        AttackInterface::TX_WS,
        {1}));

    AttackInterface::CfgDataMessage config{};
    std::strncpy(config.teamName, "reset-team", sizeof(config.teamName) - 1);
    channel.receive(asBytes(config));
    channel.receive({AttackInterface::CT_DATA});
    channel.receive({0xFF});

    channel.clearSentMessages();
    float value = 8.0F;
    attack.txData(1, AttackInterface::TX_WS, &value);
    REQUIRE(channel.sentMessages().empty());
    REQUIRE(attack.overwrite(1, AttackInterface::TX_WS, value) == AttackInterface::AI_DISABLED);
}
