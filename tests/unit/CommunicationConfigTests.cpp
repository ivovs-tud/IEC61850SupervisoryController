#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "sc/communication/CommunicationConfig.hpp"
#include "sc/runtime/RuntimeConfig.hpp"

using namespace std::chrono_literals;

TEST_CASE("communication configuration is derived from validated runtime settings") {
    sc::runtime::RuntimeConfig runtime = sc::runtime::defaultRuntimeConfig();
    runtime.hmi.period = 175ms;
    runtime.hmi.windowSize = 42;
    runtime.hmi.publisherEndpoint = "tcp://127.0.0.1:6001";
    runtime.hmi.commandEndpoint = "tcp://127.0.0.1:6002";
    runtime.monitoring.alarmAcknowledgementEnabled = true;
    runtime.communication.attackInterface.port = 6102;
    runtime.communication.mms.reports = {
        sc::runtime::ReportConfig{"disabled"},
        sc::runtime::ReportConfig{"selected"},
    };
    runtime.communication.mms.reports[0].enabled = false;
    runtime.communication.mms.reports[1].integrityPeriod = 750ms;
    runtime.communication.mms.reports[1].dataSetReference = "LD0$selected";
    runtime.communication.mms.reports[1].controlBlockReference = "LD0$RP$selected";
    runtime.turbines.resize(2);

    const auto config = sc::communication::makeCommunicationConfig(runtime);

    REQUIRE(config.hmi.period == 175ms);
    REQUIRE(config.hmi.windowSize == 42);
    REQUIRE(config.hmi.publisherEndpoint == "tcp://127.0.0.1:6001");
    REQUIRE(config.hmi.commandEndpoint == "tcp://127.0.0.1:6002");
    REQUIRE(config.hmi.alarmAcknowledgementEnabled);
    REQUIRE(config.attackInterface.port == 6102);
    REQUIRE(config.mms.turbines.size() == 2);
    REQUIRE(config.mms.reportingEnabled);
    REQUIRE(config.mms.reportTriggerPeriod == 750ms);
    REQUIRE(config.mms.reportDataSetReference == "LD0$selected");
    REQUIRE(config.mms.reportControlBlockReference == "LD0$RP$selected");
}
