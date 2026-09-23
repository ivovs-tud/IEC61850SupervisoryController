#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/DataHistorian.hpp"
#include "sc/protocol/AttackProtocol.hpp"

namespace {

std::vector<uint8_t> fromHex(const std::string& value) {
    std::vector<uint8_t> bytes;
    bytes.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        bytes.push_back(static_cast<uint8_t>(std::stoul(value.substr(index, 2), nullptr, 16)));
    }
    return bytes;
}

} // namespace

TEST_CASE("attack protocol retains the documented wire bytes") {
    using namespace sc::protocol::attack;

    REQUIRE(encode(TxDataMessage{2, AttackInterface::SignalType::YAW_ANGLE, 1, 12.5F}) ==
            fromHex("01020000050000000100000000004841"));
    REQUIRE(encode(RqDataMessage{3, AttackInterface::SignalType::POWER, 1000, 1500}) ==
            fromHex("0203000004000000e803000000000000dc05000000000000"));
    REQUIRE(encode(AtDataMessage{3, AttackInterface::SignalType::POWER, 1100, -7.25F}) ==
            fromHex("04030000040000004c040000000000000000e8c000000000"));
    REQUIRE(encode(CtDataMessage{
                ControlSignal::FDI,
                AttackInterface::SignalType::YAW_ANGLE,
                {1, 0, 1, 0, 0, 0, 0, 0, 0}}) ==
            fromHex("080000000200000005000000010001000000000000"));

    const auto configuration = encode(CfgDataMessage{"PythonAttackClient", 7, 2});
    REQUIRE(configuration.size() == CFG_DATA_SIZE);
    REQUIRE(configuration[0] == static_cast<uint8_t>(MessageType::CFG_DATA));
    REQUIRE(configuration[260] == 7);
    REQUIRE(configuration[264] == 2);

    REQUIRE(encode(SimCtrlMessage{true}) == fromHex("2001"));
    REQUIRE(encode(HeartbeatMessage{}) == fromHex("40"));
    REQUIRE(encode(ReleaseMessage{}) == fromHex("80"));
}

TEST_CASE("historian record retains the documented native ABI") {
    STATIC_REQUIRE(sizeof(DH_TCP_DATA) == 56);
    STATIC_REQUIRE(offsetof(DH_TCP_DATA, nID) == 0);
    STATIC_REQUIRE(offsetof(DH_TCP_DATA, nUnixTime) == 8);
    STATIC_REQUIRE(offsetof(DH_TCP_DATA, YwAng) == 16);
    STATIC_REQUIRE(offsetof(DH_TCP_DATA, PitchAngleSpt) == 48);
}
