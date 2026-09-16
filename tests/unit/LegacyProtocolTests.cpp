#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

#include "common/DataHistorian.hpp"
#include "communication/AttackInterface.hpp"

TEST_CASE("legacy attack protocol retains the documented native ABI") {
    using namespace AttackInterface;

    STATIC_REQUIRE(sizeof(DataHeader) == 4);
    STATIC_REQUIRE(sizeof(TxDataType) == 4);
    STATIC_REQUIRE(sizeof(ControlSignal) == 4);

    STATIC_REQUIRE(sizeof(TxDataMessage) == 16);
    STATIC_REQUIRE(offsetof(TxDataMessage, header) == 0);
    STATIC_REQUIRE(offsetof(TxDataMessage, turbineId) == 1);
    STATIC_REQUIRE(offsetof(TxDataMessage, dataType) == 4);
    STATIC_REQUIRE(offsetof(TxDataMessage, payload_length) == 8);
    STATIC_REQUIRE(offsetof(TxDataMessage, value) == 12);

    STATIC_REQUIRE(sizeof(RqDataMessage) == 24);
    STATIC_REQUIRE(offsetof(RqDataMessage, turbineId) == 1);
    STATIC_REQUIRE(offsetof(RqDataMessage, dataType) == 4);
    STATIC_REQUIRE(offsetof(RqDataMessage, rq_time) == 8);
    STATIC_REQUIRE(offsetof(RqDataMessage, exp_time) == 16);

    STATIC_REQUIRE(sizeof(AtDataMessage) == 24);
    STATIC_REQUIRE(offsetof(AtDataMessage, turbineId) == 1);
    STATIC_REQUIRE(offsetof(AtDataMessage, dataType) == 4);
    STATIC_REQUIRE(offsetof(AtDataMessage, at_time) == 8);
    STATIC_REQUIRE(offsetof(AtDataMessage, fake_value) == 16);

    STATIC_REQUIRE(sizeof(CfgDataMessage) == 268);
    STATIC_REQUIRE(offsetof(CfgDataMessage, teamName) == 1);
    STATIC_REQUIRE(offsetof(CfgDataMessage, scenarioId) == 260);
    STATIC_REQUIRE(offsetof(CfgDataMessage, turbineController) == 264);

    STATIC_REQUIRE(sizeof(SimCtrlMessage) == 2);
    STATIC_REQUIRE(offsetof(SimCtrlMessage, simStart) == 1);
}

TEST_CASE("legacy historian record retains the documented native ABI") {
    STATIC_REQUIRE(sizeof(DH_TCP_DATA) == 56);
    STATIC_REQUIRE(offsetof(DH_TCP_DATA, nID) == 0);
    STATIC_REQUIRE(offsetof(DH_TCP_DATA, nUnixTime) == 8);
    STATIC_REQUIRE(offsetof(DH_TCP_DATA, YwAng) == 16);
    STATIC_REQUIRE(offsetof(DH_TCP_DATA, PitchAngleSpt) == 48);
}
