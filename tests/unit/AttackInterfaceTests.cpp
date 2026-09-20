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
                                    AttackInterface::SignalType dataType,
                                    const std::vector<uint8_t>& enabled) {
    constexpr std::size_t prefixSize = 12;
    std::vector<uint8_t> bytes(prefixSize + enabled.size(), 0);
    bytes[0] = AttackInterface::CT_DATA;
    std::memcpy(bytes.data() + 4, &signal, sizeof(signal));
    std::memcpy(bytes.data() + 8, &dataType, sizeof(dataType));
    std::memcpy(bytes.data() + prefixSize, enabled.data(), enabled.size());
    return bytes;
}

void configure(FakeAttackChannel& channel, const char* label = "test / attack") {
    AttackInterface::CfgDataMessage config{};
    std::strncpy(config.teamName, label, sizeof(config.teamName) - 1);
    channel.receive(asBytes(config));
}

} // namespace

TEST_CASE("configuration starts a labelled session and simulation control is ignored") {
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(2, channel, clock);

    std::vector<std::string> auditEvents;
    attack.setAuditCallback([&](const std::string& event) { auditEvents.push_back(event); });

    AttackInterface::CfgDataMessage config{};
    std::strncpy(config.teamName, "integration-team", sizeof(config.teamName) - 1);
    config.scenarioId = 7;
    config.turbineController = 2;
    channel.receive(asBytes(config));

    REQUIRE(auditEvents.size() == 1);
    REQUIRE(auditEvents[0].find("label=\"integration-team\"") != std::string::npos);
    REQUIRE(auditEvents[0].find("event=connected") != std::string::npos);

    AttackInterface::SimCtrlMessage simulation{};
    simulation.simStart = true;
    channel.receive(asBytes(simulation));

    REQUIRE(auditEvents.size() == 1);
    REQUIRE(channel.sentMessages().empty());
}

TEST_CASE("tap control gates outgoing observations per turbine") {
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(2, channel, clock);
    configure(channel);

    channel.receive(controlMessage(
        AttackInterface::CTRL_TAP,
        AttackInterface::SignalType::YAW_ANGLE,
        {1, 0}));

    float firstYaw = 12.5F;
    float secondYaw = 19.0F;
    attack.txData(1, AttackInterface::SignalType::YAW_ANGLE, &firstYaw);
    attack.txData(2, AttackInterface::SignalType::YAW_ANGLE, &secondYaw);

    REQUIRE(channel.sentMessages().size() == 1);
    const auto& message = channel.sentMessages().front();
    REQUIRE(message.size() == sizeof(AttackInterface::TxDataMessage));
    REQUIRE(message[0] == AttackInterface::TX_DATA);
    REQUIRE(message[1] == 1);
    REQUIRE(readValue<AttackInterface::SignalType>(message, 4) == AttackInterface::SignalType::YAW_ANGLE);
    REQUIRE(readValue<float>(message, 12) == Catch::Approx(firstYaw));
}

TEST_CASE("FDI request accepts a matching response through the fake channel") {
    FakeAttackChannel channel;
    FakeClock clock(25'000);
    AttackInterface::AttackInterface attack(2, channel, clock);
    configure(channel);

    channel.receive(controlMessage(
        AttackInterface::CTRL_FDI,
        AttackInterface::SignalType::YAW_SETPOINT,
        {1, 0}));

    channel.setSendObserver([&](const std::vector<uint8_t>& requestBytes) {
        if (requestBytes[0] != AttackInterface::RQ_DATA) {
            return;
        }

        AttackInterface::AtDataMessage response{};
        response.turbineId = requestBytes[1];
        response.dataType = readValue<AttackInterface::SignalType>(requestBytes, 4);
        response.at_time = clock.unixTimeMilliseconds();
        response.fake_value = 91.25F;
        channel.receive(asBytes(response));
    });

    float value = 10.0F;
    REQUIRE(attack.overwrite(1, AttackInterface::SignalType::YAW_SETPOINT, value) == AttackInterface::AI_OK);
    REQUIRE(value == Catch::Approx(91.25F));
    REQUIRE(channel.sentMessages().size() == 1);

    const auto& request = channel.sentMessages().front();
    REQUIRE(readValue<AttackInterface::TimeStamp>(request, 8) == 25'000);
    REQUIRE(readValue<AttackInterface::TimeStamp>(request, 16) == 25'250);
}

TEST_CASE("missing FDI value returns immediately and preserves the original value") {
    FakeAttackChannel channel;
    FakeClock clock(50'000);
    AttackInterface::AttackTiming timing;
    timing.requestLifetime = std::chrono::milliseconds(20);
    timing.requestRetryPeriod = std::chrono::milliseconds(5);
    AttackInterface::AttackInterface attack(1, channel, clock, timing);
    configure(channel);

    channel.receive(controlMessage(
        AttackInterface::CTRL_FDI,
        AttackInterface::SignalType::POWER,
        {1}));

    float value = 500.0F;
    REQUIRE(attack.overwrite(1, AttackInterface::SignalType::POWER, value) == AttackInterface::AI_TIMEOUT);
    REQUIRE(value == Catch::Approx(500.0F));
    REQUIRE(clock.steadyNow().time_since_epoch() == std::chrono::milliseconds(0));
    REQUIRE(channel.sentMessages().size() == 1);

    const auto& request = channel.sentMessages().front();
    REQUIRE(readValue<AttackInterface::TimeStamp>(request, 8) == 50'000);
    REQUIRE(readValue<AttackInterface::TimeStamp>(request, 16) == 50'020);
}

TEST_CASE("IEC-side overwrite reads a proactively supplied FDI value locally") {
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(1, channel, clock);
    configure(channel);
    channel.receive(controlMessage(
        AttackInterface::CTRL_FDI,
        AttackInterface::SignalType::POWER_SETPOINT,
        {1}));

    AttackInterface::AtDataMessage update{};
    update.turbineId = 1;
    update.dataType = AttackInterface::SignalType::POWER_SETPOINT;
    update.at_time = clock.unixTimeMilliseconds();
    update.fake_value = 125.0F;
    channel.receive(asBytes(update));
    channel.clearSentMessages();

    float value = 500.0F;
    REQUIRE(attack.overwrite(1, AttackInterface::SignalType::POWER_SETPOINT, value) == AttackInterface::AI_OK);
    REQUIRE(value == Catch::Approx(125.0F));
    REQUIRE(channel.sentMessages().empty());
    REQUIRE(clock.steadyNow().time_since_epoch() == std::chrono::milliseconds(0));
}

TEST_CASE("reconfiguration and malformed control messages leave tapping disabled") {
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(1, channel, clock);
    configure(channel, "first attack");

    channel.receive(controlMessage(
        AttackInterface::CTRL_TAP,
        AttackInterface::SignalType::WIND_SPEED,
        {1}));

    AttackInterface::CfgDataMessage config{};
    std::strncpy(config.teamName, "reset-team", sizeof(config.teamName) - 1);
    channel.receive(asBytes(config));
    channel.receive({AttackInterface::CT_DATA});
    channel.receive({0xFF});

    channel.clearSentMessages();
    float value = 8.0F;
    attack.txData(1, AttackInterface::SignalType::WIND_SPEED, &value);
    REQUIRE(channel.sentMessages().empty());
    REQUIRE(attack.overwrite(1, AttackInterface::SignalType::WIND_SPEED, value) == AttackInterface::AI_DISABLED);
}

TEST_CASE("heartbeat lease expiry revokes attack controls and records disconnect") {
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackTiming timing;
    timing.sessionLeaseTimeout = std::chrono::milliseconds(750);
    AttackInterface::AttackInterface attack(1, channel, clock, timing);
    std::vector<std::string> auditEvents;
    attack.setAuditCallback([&](const std::string& event) { auditEvents.push_back(event); });
    configure(channel, "lease test");
    channel.receive(controlMessage(
        AttackInterface::CTRL_TAP,
        AttackInterface::SignalType::WIND_SPEED,
        {1}));

    clock.advance(std::chrono::milliseconds(500));
    channel.receive(asBytes(AttackInterface::HeartbeatMessage{}));
    clock.advance(std::chrono::milliseconds(500));
    channel.checkLease();

    float value = 8.0F;
    attack.txData(1, AttackInterface::SignalType::WIND_SPEED, &value);
    REQUIRE(channel.sentMessages().size() == 1);

    clock.advance(std::chrono::milliseconds(250));
    channel.checkLease();
    channel.clearSentMessages();
    attack.txData(1, AttackInterface::SignalType::WIND_SPEED, &value);
    REQUIRE(channel.sentMessages().empty());
    REQUIRE(auditEvents.back().find("event=disconnected;reason=lease timeout") != std::string::npos);
}

TEST_CASE("explicit release revokes controls through the same cleanup path") {
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(1, channel, clock);
    std::vector<std::string> auditEvents;
    attack.setAuditCallback([&](const std::string& event) { auditEvents.push_back(event); });
    configure(channel, "release test");
    channel.receive(controlMessage(
        AttackInterface::CTRL_TAP,
        AttackInterface::SignalType::WIND_SPEED,
        {1}));
    channel.receive(asBytes(AttackInterface::ReleaseMessage{}));
    const auto eventCount = auditEvents.size();
    channel.receive(asBytes(AttackInterface::ReleaseMessage{}));

    float value = 8.0F;
    attack.txData(1, AttackInterface::SignalType::WIND_SPEED, &value);
    REQUIRE(channel.sentMessages().empty());
    REQUIRE(auditEvents.size() == eventCount);
    REQUIRE(auditEvents.back().find("event=disconnected;reason=client release") != std::string::npos);
}

TEST_CASE("control audit identifies signal and turbine and logs attack start once") {
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(2, channel, clock);
    std::vector<std::string> auditEvents;
    attack.setAuditCallback([&](const std::string& event) { auditEvents.push_back(event); });
    configure(channel, "audit test");

    channel.receive(controlMessage(
        AttackInterface::CTRL_FDI,
        AttackInterface::SignalType::YAW_SETPOINT,
        {1, 0}));
    channel.receive(controlMessage(
        AttackInterface::CTRL_TAP,
        AttackInterface::SignalType::POWER,
        {1, 0}));

    int startEvents = 0;
    for (const auto& event : auditEvents) {
        if (event.find("event=attack_started") != std::string::npos) ++startEvents;
    }
    REQUIRE(startEvents == 1);
    REQUIRE(auditEvents[1].find("event=fdi;signal=Yaw setpoint;turbine=1;enabled=true") != std::string::npos);
    REQUIRE(auditEvents[2].find("event=fdi;signal=Yaw setpoint;turbine=2;enabled=false") != std::string::npos);
}
