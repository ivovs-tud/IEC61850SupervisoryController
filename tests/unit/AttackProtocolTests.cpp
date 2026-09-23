#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sc/protocol/AttackProtocol.hpp"

namespace {

using sc::protocol::attack::Bytes;

std::vector<Bytes> messages(std::size_t turbineCount) {
    using namespace sc::protocol::attack;
    return {
        encode(TxDataMessage{2, AttackInterface::SignalType::YAW_ANGLE, 1, 12.5F}),
        encode(RqDataMessage{1, AttackInterface::SignalType::POWER, 1000, 1250}),
        encode(AtDataMessage{1, AttackInterface::SignalType::POWER, 1100, -7.25F}),
        encode(CtDataMessage{ControlSignal::TAP, AttackInterface::SignalType::WIND_SPEED, std::vector<uint8_t>(turbineCount, 1)}),
        encode(CfgDataMessage{"decoder test", 7, 2}),
        encode(SimCtrlMessage{true}),
        encode(HeartbeatMessage{}),
        encode(ReleaseMessage{}),
    };
}

} // namespace

TEST_CASE("attack stream decoder accepts every split boundary") {
    constexpr std::size_t turbineCount = 3;
    for (const auto& expected : messages(turbineCount)) {
        for (std::size_t split = 0; split <= expected.size(); ++split) {
            sc::protocol::attack::StreamDecoder decoder(turbineCount);
            const auto first = decoder.push(expected.data(), split);
            const auto second = decoder.push(expected.data() + split, expected.size() - split);

            REQUIRE(first.size() + second.size() == 1);
            REQUIRE((first.empty() ? second.front() : first.front()) == expected);
            REQUIRE(decoder.bufferedBytes() == 0);
            REQUIRE_NOTHROW(decoder.finish());
        }
    }
}

TEST_CASE("attack stream decoder returns coalesced messages in order") {
    constexpr std::size_t turbineCount = 2;
    const auto expected = messages(turbineCount);
    Bytes combined;
    for (const auto& message : expected) combined.insert(combined.end(), message.begin(), message.end());

    sc::protocol::attack::StreamDecoder decoder(turbineCount);
    REQUIRE(decoder.push(combined) == expected);
}

TEST_CASE("attack stream decoder rejects unknown and truncated messages") {
    sc::protocol::attack::StreamDecoder unknownHeader(2);
    REQUIRE_THROWS_AS(unknownHeader.push(Bytes{0xFF}), sc::protocol::attack::ProtocolError);

    auto invalidControl = sc::protocol::attack::encode(sc::protocol::attack::CtDataMessage{
        sc::protocol::attack::ControlSignal::TAP,
        AttackInterface::SignalType::WIND_SPEED,
        {1, 2}});
    sc::protocol::attack::StreamDecoder invalidFlags(2);
    REQUIRE_THROWS_AS(invalidFlags.push(invalidControl), sc::protocol::attack::ProtocolError);

    sc::protocol::attack::StreamDecoder truncated(2);
    const auto configuration = sc::protocol::attack::encode(
        sc::protocol::attack::CfgDataMessage{"truncated", 0, 0});
    REQUIRE(truncated.push(configuration.data(), configuration.size() - 1).empty());
    REQUIRE_THROWS_AS(truncated.finish(), sc::protocol::attack::ProtocolError);
}

TEST_CASE("attack stream decoder enforces its byte limit") {
    sc::protocol::attack::StreamDecoder decoder(2, 8);
    const auto configuration = sc::protocol::attack::encode(
        sc::protocol::attack::CfgDataMessage{"oversized", 0, 0});
    REQUIRE_THROWS_AS(decoder.push(configuration.data(), 9), sc::protocol::attack::ProtocolError);
}

TEST_CASE("control message size follows the configured turbine count") {
    using namespace sc::protocol::attack;
    StreamDecoder decoder(3);
    const auto control = encode(CtDataMessage{
        ControlSignal::FDI, AttackInterface::SignalType::YAW_SETPOINT, {1, 0, 1}});

    REQUIRE(control.size() == CT_DATA_PREFIX_SIZE + 3);
    REQUIRE(decoder.push(control.data(), control.size() - 1).empty());
    const auto decoded = decoder.push(control.data() + control.size() - 1, 1);
    REQUIRE(decoded == std::vector<Bytes>{control});
}
