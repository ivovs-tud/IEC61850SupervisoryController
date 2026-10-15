#include "sc/communication/attack/AttackInterface.hpp"
#include "support/FakeAttackChannel.hpp"
#include "support/FakeClock.hpp"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

template <typename Value> Value readValue(const std::vector<uint8_t>& bytes, std::size_t offset)
{
    Value value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(Value));
    return value;
}

std::vector<uint8_t> controlMessage(AttackInterface::ControlSignal signal, AttackInterface::SignalType dataType,
                                    const std::vector<uint8_t>& enabled)
{
    return sc::protocol::attack::encode(AttackInterface::CtDataMessage{signal, dataType, enabled});
}

void configure(FakeAttackChannel& channel, const char* label = "test / attack")
{
    channel.receive(sc::protocol::attack::encode(AttackInterface::CfgDataMessage{label, 0, 0}));
    channel.clearSentMessages();
}

} // namespace

TEST_CASE("configuration starts a labelled session and simulation control is ignored")
{
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
    REQUIRE(channel.sentMessages() == std::vector<std::vector<uint8_t>>{sc::protocol::attack::encode(config)});
}

TEST_CASE("tap control gates outgoing observations per turbine")
{
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(2, channel, clock);
    configure(channel);

    channel.receive(controlMessage(AttackInterface::ControlSignal::TAP, AttackInterface::SignalType::YAW_ANGLE, {1, 0}));

    float firstYaw = 12.5F;
    float secondYaw = 19.0F;
    attack.txData(1, AttackInterface::SignalType::YAW_ANGLE, firstYaw);
    attack.txData(2, AttackInterface::SignalType::YAW_ANGLE, secondYaw);

    REQUIRE(channel.sentMessages().size() == 1);
    const auto& message = channel.sentMessages().front();
    REQUIRE(message.size() == sc::protocol::attack::TX_DATA_SIZE);
    REQUIRE(message[0] == static_cast<uint8_t>(AttackInterface::MessageType::TX_DATA));
    REQUIRE(message[1] == 1);
    REQUIRE(readValue<AttackInterface::SignalType>(message, 4) == AttackInterface::SignalType::YAW_ANGLE);
    REQUIRE(readValue<float>(message, 12) == Catch::Approx(firstYaw));
}

TEST_CASE("integer observations retain their type on the attack interface")
{
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(1, channel, clock);
    configure(channel);

    channel.receive(controlMessage(AttackInterface::ControlSignal::TAP, AttackInterface::SignalType::OPERATION_COMMAND, {1}));

    attack.processValue(1, AttackInterface::SignalType::OPERATION_COMMAND, uint32_t{3});

    REQUIRE(channel.sentMessages().size() == 1);
    const auto message = std::get<AttackInterface::TxDataMessage>(sc::protocol::attack::decode(channel.sentMessages().front(), 1));
    REQUIRE(message.dataType == AttackInterface::SignalType::OPERATION_COMMAND);
    REQUIRE(std::get<uint32_t>(message.value) == 3);
}

TEST_CASE("FDI request accepts a matching response through the fake channel")
{
    FakeAttackChannel channel;
    FakeClock clock(25'000);
    AttackInterface::AttackInterface attack(2, channel, clock);
    configure(channel);

    channel.receive(controlMessage(AttackInterface::ControlSignal::FDI, AttackInterface::SignalType::YAW_SETPOINT, {1, 0}));

    channel.setSendObserver([&](const std::vector<uint8_t>& requestBytes) {
        if (requestBytes[0] != static_cast<uint8_t>(AttackInterface::MessageType::RQ_DATA)) {
            return;
        }

        channel.receive(sc::protocol::attack::encode(
            AttackInterface::AtDataMessage{requestBytes[1], readValue<AttackInterface::SignalType>(requestBytes, 4),
                                           readValue<AttackInterface::TimeStamp>(requestBytes, 8), 91.25F}));
    });

    float value = 10.0F;
    REQUIRE(attack.overwrite(1, AttackInterface::SignalType::YAW_SETPOINT, value) == AttackInterface::AI_OK);
    REQUIRE(value == Catch::Approx(91.25F));
    REQUIRE(channel.sentMessages().size() == 1);

    const auto& request = channel.sentMessages().front();
    REQUIRE(readValue<AttackInterface::TimeStamp>(request, 8) == 25'000);
    REQUIRE(readValue<AttackInterface::TimeStamp>(request, 16) == 25'250);
}

TEST_CASE("tap and FDI emit one observation before the replacement request")
{
    FakeAttackChannel channel;
    FakeClock clock(25'000);
    AttackInterface::AttackInterface attack(1, channel, clock);
    configure(channel);

    channel.receive(controlMessage(AttackInterface::ControlSignal::TAP, AttackInterface::SignalType::WIND_SPEED, {1}));
    channel.receive(controlMessage(AttackInterface::ControlSignal::FDI, AttackInterface::SignalType::WIND_SPEED, {1}));

    channel.setSendObserver([&](const std::vector<uint8_t>& message) {
        if (message[0] == static_cast<uint8_t>(AttackInterface::MessageType::RQ_DATA)) {
            channel.receive(sc::protocol::attack::encode(AttackInterface::AtDataMessage{
                message[1], readValue<AttackInterface::SignalType>(message, 4), readValue<AttackInterface::TimeStamp>(message, 8), 12.0F}));
        }
    });

    float value = 8.0F;
    REQUIRE(attack.processValue(1, AttackInterface::SignalType::WIND_SPEED, value) == AttackInterface::AI_OK);

    REQUIRE(value == Catch::Approx(12.0F));
    REQUIRE(channel.sentMessages().size() == 2);
    REQUIRE(channel.sentMessages()[0][0] == static_cast<uint8_t>(AttackInterface::MessageType::TX_DATA));
    REQUIRE(channel.sentMessages()[1][0] == static_cast<uint8_t>(AttackInterface::MessageType::RQ_DATA));
}

TEST_CASE("process value still emits a tap observation when FDI is disabled")
{
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(1, channel, clock);
    configure(channel);

    channel.receive(controlMessage(AttackInterface::ControlSignal::TAP, AttackInterface::SignalType::WIND_SPEED, {1}));

    float value = 8.0F;
    REQUIRE(attack.processValue(1, AttackInterface::SignalType::WIND_SPEED, value) == AttackInterface::AI_DISABLED);
    REQUIRE(channel.sentMessages().size() == 1);
    REQUIRE(channel.sentMessages()[0][0] == static_cast<uint8_t>(AttackInterface::MessageType::TX_DATA));
}

TEST_CASE("FDI response timeout preserves the original value")
{
    FakeAttackChannel channel;
    FakeClock clock(50'000);
    AttackInterface::AttackTiming timing;
    timing.requestLifetime = std::chrono::milliseconds(20);
    AttackInterface::AttackInterface attack(1, channel, clock, timing);
    configure(channel);

    channel.receive(controlMessage(AttackInterface::ControlSignal::FDI, AttackInterface::SignalType::POWER, {1}));

    float value = 500.0F;
    REQUIRE(attack.overwrite(1, AttackInterface::SignalType::POWER, value) == AttackInterface::AI_TIMEOUT);
    REQUIRE(value == Catch::Approx(500.0F));
    REQUIRE(clock.steadyNow().time_since_epoch() == std::chrono::milliseconds(0));
    REQUIRE(channel.sentMessages().size() == 1);

    const auto& request = channel.sentMessages().front();
    REQUIRE(readValue<AttackInterface::TimeStamp>(request, 8) == 50'000);
    REQUIRE(readValue<AttackInterface::TimeStamp>(request, 16) == 50'020);
}

TEST_CASE("unsolicited FDI values are not applied")
{
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackTiming timing;
    timing.requestLifetime = std::chrono::milliseconds(1);
    AttackInterface::AttackInterface attack(1, channel, clock, timing);
    configure(channel);
    channel.receive(controlMessage(AttackInterface::ControlSignal::FDI, AttackInterface::SignalType::POWER_SETPOINT, {1}));

    channel.receive(sc::protocol::attack::encode(
        AttackInterface::AtDataMessage{1, AttackInterface::SignalType::POWER_SETPOINT, clock.unixTimeMilliseconds(), 125.0F}));
    channel.clearSentMessages();

    float value = 500.0F;
    REQUIRE(attack.overwrite(1, AttackInterface::SignalType::POWER_SETPOINT, value) == AttackInterface::AI_TIMEOUT);
    REQUIRE(value == Catch::Approx(500.0F));
    REQUIRE(channel.sentMessages().size() == 1);
}

TEST_CASE("NaN and timeout can reuse the last finite FDI response")
{
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackTiming timing;
    timing.requestLifetime = std::chrono::milliseconds(1);
    AttackInterface::AttackInterface attack(1, channel, clock, timing);
    configure(channel);
    channel.receive(controlMessage(AttackInterface::ControlSignal::FDI, AttackInterface::SignalType::POWER_SETPOINT, {1}));

    int requestCount = 0;
    channel.setSendObserver([&](const std::vector<uint8_t>& request) {
        if (request[0] != static_cast<uint8_t>(AttackInterface::MessageType::RQ_DATA))
            return;
        ++requestCount;
        if (requestCount > 2)
            return;
        const float response = requestCount == 1 ? 91.0F : std::numeric_limits<float>::quiet_NaN();
        channel.receive(sc::protocol::attack::encode(AttackInterface::AtDataMessage{
            request[1], readValue<AttackInterface::SignalType>(request, 4), readValue<AttackInterface::TimeStamp>(request, 8), response}));
    });

    float value = 10.0F;
    REQUIRE(attack.overwrite(1, AttackInterface::SignalType::POWER_SETPOINT, value) == AttackInterface::AI_OK);
    REQUIRE(value == Catch::Approx(91.0F));

    value = 20.0F;
    REQUIRE(attack.overwrite(1, AttackInterface::SignalType::POWER_SETPOINT, value) == AttackInterface::AI_OK);
    REQUIRE(value == Catch::Approx(91.0F));

    value = 30.0F;
    REQUIRE(attack.overwrite(1, AttackInterface::SignalType::POWER_SETPOINT, value) == AttackInterface::AI_OK);
    REQUIRE(value == Catch::Approx(91.0F));
}

TEST_CASE("cached FDI fallback can be disabled")
{
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackTiming timing;
    timing.requestLifetime = std::chrono::milliseconds(1);
    timing.reuseLastFdiValueOnFailure = false;
    AttackInterface::AttackInterface attack(1, channel, clock, timing);
    configure(channel);
    channel.receive(controlMessage(AttackInterface::ControlSignal::FDI, AttackInterface::SignalType::POWER_SETPOINT, {1}));

    int requestCount = 0;
    channel.setSendObserver([&](const std::vector<uint8_t>& request) {
        if (request[0] != static_cast<uint8_t>(AttackInterface::MessageType::RQ_DATA))
            return;
        ++requestCount;
        if (requestCount > 2)
            return;
        const float response = requestCount == 1 ? 91.0F : std::numeric_limits<float>::quiet_NaN();
        channel.receive(sc::protocol::attack::encode(AttackInterface::AtDataMessage{
            request[1], readValue<AttackInterface::SignalType>(request, 4), readValue<AttackInterface::TimeStamp>(request, 8), response}));
    });

    float value = 10.0F;
    REQUIRE(attack.overwrite(1, AttackInterface::SignalType::POWER_SETPOINT, value) == AttackInterface::AI_OK);
    REQUIRE(value == Catch::Approx(91.0F));

    value = 20.0F;
    REQUIRE(attack.overwrite(1, AttackInterface::SignalType::POWER_SETPOINT, value) == AttackInterface::AI_TIMEOUT);
    REQUIRE(value == Catch::Approx(20.0F));

    value = 30.0F;
    REQUIRE(attack.overwrite(1, AttackInterface::SignalType::POWER_SETPOINT, value) == AttackInterface::AI_TIMEOUT);
    REQUIRE(value == Catch::Approx(30.0F));
}

TEST_CASE("reconfiguration and malformed control messages leave tapping disabled")
{
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(1, channel, clock);
    configure(channel, "first attack");

    channel.receive(controlMessage(AttackInterface::ControlSignal::TAP, AttackInterface::SignalType::WIND_SPEED, {1}));

    channel.receive(sc::protocol::attack::encode(AttackInterface::CfgDataMessage{"reset-team", 0, 0}));
    channel.receive({static_cast<uint8_t>(AttackInterface::MessageType::CT_DATA)});
    channel.receive({0xFF});

    channel.clearSentMessages();
    float value = 8.0F;
    attack.txData(1, AttackInterface::SignalType::WIND_SPEED, value);
    REQUIRE(channel.sentMessages().empty());
    REQUIRE(attack.overwrite(1, AttackInterface::SignalType::WIND_SPEED, value) == AttackInterface::AI_DISABLED);
}

TEST_CASE("heartbeat lease expiry revokes attack controls and records disconnect")
{
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackTiming timing;
    timing.sessionLeaseTimeout = std::chrono::milliseconds(750);
    AttackInterface::AttackInterface attack(1, channel, clock, timing);
    std::vector<std::string> auditEvents;
    attack.setAuditCallback([&](const std::string& event) { auditEvents.push_back(event); });
    configure(channel, "lease test");
    channel.receive(controlMessage(AttackInterface::ControlSignal::TAP, AttackInterface::SignalType::WIND_SPEED, {1}));

    clock.advance(std::chrono::milliseconds(500));
    channel.receive(sc::protocol::attack::encode(AttackInterface::HeartbeatMessage{}));
    clock.advance(std::chrono::milliseconds(500));
    channel.checkLease();

    float value = 8.0F;
    attack.txData(1, AttackInterface::SignalType::WIND_SPEED, value);
    REQUIRE(channel.sentMessages().size() == 1);

    clock.advance(std::chrono::milliseconds(250));
    channel.checkLease();
    channel.clearSentMessages();
    attack.txData(1, AttackInterface::SignalType::WIND_SPEED, value);
    REQUIRE(channel.sentMessages().empty());
    REQUIRE(auditEvents.back().find("event=disconnected;reason=lease timeout") != std::string::npos);
}

TEST_CASE("explicit release revokes controls through the same cleanup path")
{
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(1, channel, clock);
    std::vector<std::string> auditEvents;
    attack.setAuditCallback([&](const std::string& event) { auditEvents.push_back(event); });
    configure(channel, "release test");
    channel.receive(controlMessage(AttackInterface::ControlSignal::TAP, AttackInterface::SignalType::WIND_SPEED, {1}));
    channel.receive(sc::protocol::attack::encode(AttackInterface::ReleaseMessage{}));
    const auto eventCount = auditEvents.size();
    channel.receive(sc::protocol::attack::encode(AttackInterface::ReleaseMessage{}));

    float value = 8.0F;
    attack.txData(1, AttackInterface::SignalType::WIND_SPEED, value);
    REQUIRE(channel.sentMessages().empty());
    REQUIRE(auditEvents.size() == eventCount);
    REQUIRE(auditEvents.back().find("event=disconnected;reason=client release") != std::string::npos);
}

TEST_CASE("transport disconnect revokes controls through the same cleanup path")
{
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(1, channel, clock);
    std::vector<std::string> auditEvents;
    attack.setAuditCallback([&](const std::string& event) { auditEvents.push_back(event); });
    configure(channel, "disconnect test");
    channel.receive(controlMessage(AttackInterface::ControlSignal::TAP, AttackInterface::SignalType::WIND_SPEED, {1}));

    channel.disconnect("peer closed connection");
    float value = 8.0F;
    attack.txData(1, AttackInterface::SignalType::WIND_SPEED, value);

    REQUIRE(channel.sentMessages().empty());
    REQUIRE(auditEvents.back().find("event=disconnected;reason=peer closed connection") != std::string::npos);
}

TEST_CASE("cleanup reveals the latest authoritative value and runs only once")
{
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(1, channel, clock);
    std::vector<std::string> auditEvents;
    attack.setAuditCallback([&](const std::string& event) { auditEvents.push_back(event); });
    configure(channel, "restoration test");
    channel.receive(controlMessage(AttackInterface::ControlSignal::FDI, AttackInterface::SignalType::POWER_SETPOINT, {1}));
    channel.setSendObserver([&](const std::vector<uint8_t>& request) {
        if (request[0] != static_cast<uint8_t>(AttackInterface::MessageType::RQ_DATA))
            return;
        channel.receive(sc::protocol::attack::encode(AttackInterface::AtDataMessage{
            request[1], readValue<AttackInterface::SignalType>(request, 4), readValue<AttackInterface::TimeStamp>(request, 8), 91.0F}));
    });

    float value = 10.0F;
    REQUIRE(attack.overwrite(1, AttackInterface::SignalType::POWER_SETPOINT, value) == AttackInterface::AI_OK);
    REQUIRE(value == Catch::Approx(91.0F));

    channel.disconnect("connection reset");
    channel.disconnect("duplicate notification");
    value = 321.0F;
    REQUIRE(attack.overwrite(1, AttackInterface::SignalType::POWER_SETPOINT, value) == AttackInterface::AI_DISABLED);
    REQUIRE(value == Catch::Approx(321.0F));

    const auto disconnectEvents = std::count_if(auditEvents.begin(), auditEvents.end(), [](const std::string& event) {
        return event.find("event=disconnected") != std::string::npos;
    });
    REQUIRE(disconnectEvents == 1);
}

TEST_CASE("a late heartbeat cannot revive an expired attack session")
{
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackTiming timing;
    timing.sessionLeaseTimeout = std::chrono::milliseconds(250);
    AttackInterface::AttackInterface attack(1, channel, clock, timing);
    configure(channel, "expired session");
    channel.receive(controlMessage(AttackInterface::ControlSignal::TAP, AttackInterface::SignalType::WIND_SPEED, {1}));

    clock.advance(std::chrono::milliseconds(250));
    channel.checkLease();
    channel.receive(sc::protocol::attack::encode(AttackInterface::HeartbeatMessage{}));
    channel.receive(controlMessage(AttackInterface::ControlSignal::TAP, AttackInterface::SignalType::WIND_SPEED, {1}));

    channel.clearSentMessages();
    float value = 8.0F;
    attack.txData(1, AttackInterface::SignalType::WIND_SPEED, value);
    REQUIRE(channel.sentMessages().empty());
}

TEST_CASE("configuration acknowledgement failure revokes the new session")
{
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(1, channel, clock);
    std::vector<std::string> auditEvents;
    attack.setAuditCallback([&](const std::string& event) { auditEvents.push_back(event); });
    channel.setSendSucceeds(false);

    channel.receive(sc::protocol::attack::encode(AttackInterface::CfgDataMessage{"failed acknowledgement", 0, 0}));
    channel.receive(controlMessage(AttackInterface::ControlSignal::TAP, AttackInterface::SignalType::WIND_SPEED, {1}));

    channel.clearSentMessages();
    float value = 8.0F;
    attack.txData(1, AttackInterface::SignalType::WIND_SPEED, value);
    REQUIRE(channel.sentMessages().empty());
    REQUIRE(auditEvents.back().find("event=disconnected;reason=configuration acknowledgement failed") != std::string::npos);
}

TEST_CASE("control audit identifies signal and turbine and logs attack start once")
{
    FakeAttackChannel channel;
    FakeClock clock;
    AttackInterface::AttackInterface attack(2, channel, clock);
    std::vector<std::string> auditEvents;
    attack.setAuditCallback([&](const std::string& event) { auditEvents.push_back(event); });
    configure(channel, "audit test");

    channel.receive(controlMessage(AttackInterface::ControlSignal::FDI, AttackInterface::SignalType::YAW_SETPOINT, {1, 0}));
    channel.receive(controlMessage(AttackInterface::ControlSignal::TAP, AttackInterface::SignalType::POWER, {1, 0}));

    int startEvents = 0;
    for (const auto& event : auditEvents) {
        if (event.find("event=attack_started") != std::string::npos)
            ++startEvents;
    }
    REQUIRE(startEvents == 1);
    REQUIRE(auditEvents[1].find("event=fdi;signal=Yaw setpoint;turbine=1;enabled=true") != std::string::npos);
    REQUIRE(auditEvents[2].find("event=fdi;signal=Yaw setpoint;turbine=2;enabled=false") != std::string::npos);
}
