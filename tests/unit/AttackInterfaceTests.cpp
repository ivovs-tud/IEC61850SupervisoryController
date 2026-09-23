#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "communication/AttackInterface.hpp"
#include "support/FakeAttackChannel.hpp"
#include "support/FakeClock.hpp"

namespace {

template <typename Value>
Value readValue(const std::vector<uint8_t>& bytes, std::size_t offset) {
    Value value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(Value));
    return value;
}

std::vector<uint8_t> controlMessage(AttackInterface::ControlSignal signal,
                                    AttackInterface::SignalType dataType,
                                    const std::vector<uint8_t>& enabled) {
    return sc::protocol::attack::encode(AttackInterface::CtDataMessage{signal, dataType, enabled});
}

void configure(FakeAttackChannel& channel, const char* label = "test / attack") {
    channel.receive(sc::protocol::attack::encode(AttackInterface::CfgDataMessage{label, 0, 0}));
    channel.clearSentMessages();
}

} // namespace

TEST_CASE("configuration starts a labelled session and simulation control is ignored") {
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(2, channel, clock);

    std::vector<std::string> auditEvents;
    attack.setAuditCallback([&](const std::string& event) { auditEvents.push_back(event); });

    const AttackInterface::CfgDataMessage config{"integration-team", 7, 2};
    channel.receive(sc::protocol::attack::encode(config));

    REQUIRE(auditEvents.size() == 1);
    REQUIRE(auditEvents[0].find("label=\"integration-team\"") != std::string::npos);
    REQUIRE(auditEvents[0].find("event=connected") != std::string::npos);

    channel.receive(sc::protocol::attack::encode(AttackInterface::SimCtrlMessage{true}));

    REQUIRE(auditEvents.size() == 1);
    REQUIRE(channel.sentMessages() == std::vector<std::vector<uint8_t>>{
        sc::protocol::attack::encode(config)});
}

TEST_CASE("tap control gates outgoing observations per turbine") {
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(2, channel, clock);
    configure(channel);

    channel.receive(controlMessage(
        AttackInterface::ControlSignal::TAP,
        AttackInterface::SignalType::YAW_ANGLE,
        {1, 0}));

    float firstYaw = 12.5F;
    float secondYaw = 19.0F;
    attack.txData(1, AttackInterface::SignalType::YAW_ANGLE, &firstYaw);
    attack.txData(2, AttackInterface::SignalType::YAW_ANGLE, &secondYaw);

    REQUIRE(channel.sentMessages().size() == 1);
    const auto& message = channel.sentMessages().front();
    REQUIRE(message.size() == sc::protocol::attack::TX_DATA_SIZE);
    REQUIRE(message[0] == static_cast<uint8_t>(AttackInterface::MessageType::TX_DATA));
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
        AttackInterface::ControlSignal::FDI,
        AttackInterface::SignalType::YAW_SETPOINT,
        {1, 0}));

    channel.setSendObserver([&](const std::vector<uint8_t>& requestBytes) {
        if (requestBytes[0] != static_cast<uint8_t>(AttackInterface::MessageType::RQ_DATA)) {
            return;
        }

        channel.receive(sc::protocol::attack::encode(AttackInterface::AtDataMessage{
            requestBytes[1],
            readValue<AttackInterface::SignalType>(requestBytes, 4),
            clock.unixTimeMilliseconds(),
            91.25F}));
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
        AttackInterface::ControlSignal::FDI,
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
        AttackInterface::ControlSignal::FDI,
        AttackInterface::SignalType::POWER_SETPOINT,
        {1}));

    channel.receive(sc::protocol::attack::encode(AttackInterface::AtDataMessage{
        1,
        AttackInterface::SignalType::POWER_SETPOINT,
        clock.unixTimeMilliseconds(),
        125.0F}));
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
        AttackInterface::ControlSignal::TAP,
        AttackInterface::SignalType::WIND_SPEED,
        {1}));

    channel.receive(sc::protocol::attack::encode(AttackInterface::CfgDataMessage{"reset-team", 0, 0}));
    channel.receive({static_cast<uint8_t>(AttackInterface::MessageType::CT_DATA)});
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
        AttackInterface::ControlSignal::TAP,
        AttackInterface::SignalType::WIND_SPEED,
        {1}));

    clock.advance(std::chrono::milliseconds(500));
    channel.receive(sc::protocol::attack::encode(AttackInterface::HeartbeatMessage{}));
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
        AttackInterface::ControlSignal::TAP,
        AttackInterface::SignalType::WIND_SPEED,
        {1}));
    channel.receive(sc::protocol::attack::encode(AttackInterface::ReleaseMessage{}));
    const auto eventCount = auditEvents.size();
    channel.receive(sc::protocol::attack::encode(AttackInterface::ReleaseMessage{}));

    float value = 8.0F;
    attack.txData(1, AttackInterface::SignalType::WIND_SPEED, &value);
    REQUIRE(channel.sentMessages().empty());
    REQUIRE(auditEvents.size() == eventCount);
    REQUIRE(auditEvents.back().find("event=disconnected;reason=client release") != std::string::npos);
}

TEST_CASE("transport disconnect revokes controls through the same cleanup path") {
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(1, channel, clock);
    std::vector<std::string> auditEvents;
    attack.setAuditCallback([&](const std::string& event) { auditEvents.push_back(event); });
    configure(channel, "disconnect test");
    channel.receive(controlMessage(
        AttackInterface::ControlSignal::TAP,
        AttackInterface::SignalType::WIND_SPEED,
        {1}));

    channel.disconnect("peer closed connection");
    float value = 8.0F;
    attack.txData(1, AttackInterface::SignalType::WIND_SPEED, &value);

    REQUIRE(channel.sentMessages().empty());
    REQUIRE(auditEvents.back().find(
        "event=disconnected;reason=peer closed connection") != std::string::npos);
}

TEST_CASE("control audit identifies signal and turbine and logs attack start once") {
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(2, channel, clock);
    std::vector<std::string> auditEvents;
    attack.setAuditCallback([&](const std::string& event) { auditEvents.push_back(event); });
    configure(channel, "audit test");

    channel.receive(controlMessage(
        AttackInterface::ControlSignal::FDI,
        AttackInterface::SignalType::YAW_SETPOINT,
        {1, 0}));
    channel.receive(controlMessage(
        AttackInterface::ControlSignal::TAP,
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
