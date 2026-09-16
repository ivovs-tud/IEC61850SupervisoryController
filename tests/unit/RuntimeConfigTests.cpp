#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>

#include "common/GlobalDataStructure.hpp"
#include "sc/application/YawLut.hpp"
#include "sc/runtime/RuntimeConfig.hpp"
#include "support/TemporaryCsv.hpp"

using namespace std::chrono_literals;

TEST_CASE("runtime defaults define nine localhost MMS turbines on ports 102 through 110") {
    const auto config = sc::runtime::defaultRuntimeConfig();

    REQUIRE(config.turbines.size() == 9);
    for (std::size_t index = 0; index < config.turbines.size(); ++index) {
        const auto& endpoint = config.turbines[index];
        REQUIRE(endpoint.id == "WT" + std::to_string(index + 1));
        REQUIRE(endpoint.host == "localhost");
        REQUIRE(endpoint.port == 102 + static_cast<int>(index));
        REQUIRE(endpoint.iedName == "WTURBINE");
        REQUIRE(endpoint.logicalDevice == "LD0");
    }
    REQUIRE_NOTHROW(sc::runtime::validateRuntimeConfig(config, 9));
}

TEST_CASE("default runtime turbine list matches the shipped yaw LUT") {
    const auto config = sc::runtime::defaultRuntimeConfig();
    const sc::application::YawLut yawLut(
        (std::filesystem::path(SC_SOURCE_DIR) / "yaw_lut.csv").string());

    REQUIRE_NOTHROW(sc::runtime::validateRuntimeConfig(config, yawLut.turbineCount()));
}

TEST_CASE("shipped versioned JSON example loads and matches its yaw LUT") {
    const auto config = sc::runtime::loadRuntimeConfig(
        std::filesystem::path(SC_SOURCE_DIR) / "examples/configs/default.json");
    const sc::application::YawLut yawLut(config.control.yawLutCsvPath.string());

    REQUIRE(config.turbines.size() == 9);
    REQUIRE(config.communication.mms.reports.size() == 2);
    REQUIRE(config.communication.mms.reports[0].enabled);
    REQUIRE_FALSE(config.communication.mms.reports[1].enabled);
    REQUIRE_NOTHROW(sc::runtime::validateRuntimeConfig(config, yawLut.turbineCount()));
}

TEST_CASE("runtime JSON overrides defaults and resolves configured paths") {
    const sc::test::TemporaryCsv file(
        "{\n"
        "  \"turbines\": [\n"
        "    {\"host\": \"10.0.0.1\", \"port\": 120, \"ied_name\": \"A\", \"logical_device\": \"LD1\"},\n"
        "    {\"host\": \"10.0.0.2\", \"port\": 121}\n"
        "  ],\n"
        "  \"tasks\": {\"control_period_ms\": 25},\n"
        "  \"control\": {\"yaw_lut_csv\": \"lut.csv\"},\n"
        "  \"hmi\": {\"window_size\": 42},\n"
        "  \"historian\": {\"output_directory\": \"logs\"},\n"
        "  \"communication\": {\"mms\": {\"reporting_enabled\": false}}\n"
        "}\n");

    const auto config = sc::runtime::loadRuntimeConfig(file.path());
    const std::filesystem::path parent = std::filesystem::path(file.path()).parent_path();

    REQUIRE(config.turbines.size() == 2);
    REQUIRE(config.turbines[0].host == "10.0.0.1");
    REQUIRE(config.turbines[0].iedName == "A");
    REQUIRE(config.turbines[1].iedName == "WTURBINE");
    REQUIRE(config.tasks.controlPeriod == 25ms);
    REQUIRE(config.hmi.windowSize == 42);
    REQUIRE(config.control.yawLutCsvPath == parent / "lut.csv");
    REQUIRE(config.historian.outputDirectory == parent / "logs");
    REQUIRE_FALSE(config.communication.mms.reports.front().enabled);
}

TEST_CASE("runtime JSON accepts future turbine metadata and named report definitions") {
    const sc::test::TemporaryCsv file(
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"turbines\": [\n"
        "    {\n"
        "      \"id\": \"north-01\",\n"
        "      \"mms\": {\"host\": \"10.0.0.1\", \"port\": 102},\n"
        "      \"model\": {\"name\": \"custom\", \"parameters\": {\"rated_power_w\": 6000000}},\n"
        "      \"metadata\": {\"location\": \"north row\"},\n"
        "      \"extensions\": {\"vendor.example\": {\"asset_id\": 17}}\n"
        "    }\n"
        "  ],\n"
        "  \"communication\": {\"mms\": {\"reports\": [\n"
        "    {\n"
        "      \"name\": \"operational\", \"enabled\": true,\n"
        "      \"integrity_period_ms\": 750,\n"
        "      \"dataset_reference\": \"LD0$dataset\",\n"
        "      \"control_block_reference\": \"LD0$RP$report\",\n"
        "      \"data_references\": [\"WTUR1$MX$W\"],\n"
        "      \"trigger_options\": {\"data_update\": true}\n"
        "    },\n"
        "    {\"name\": \"diagnostics\", \"enabled\": false}\n"
        "  ]}},\n"
        "  \"extensions\": {\"site.example\": {\"region\": \"test\"}}\n"
        "}\n");

    const auto config = sc::runtime::loadRuntimeConfig(file.path());

    REQUIRE(config.schemaVersion == 1);
    REQUIRE(config.turbines.size() == 1);
    REQUIRE(config.turbines[0].id == "north-01");
    REQUIRE(config.turbines[0].host == "10.0.0.1");
    REQUIRE(config.communication.mms.reports.size() == 2);
    REQUIRE(config.communication.mms.reports[0].name == "operational");
    REQUIRE(config.communication.mms.reports[0].integrityPeriod == 750ms);
    REQUIRE(config.communication.mms.reports[0].dataReferences ==
            std::vector<std::string>{"WTUR1$MX$W"});
    REQUIRE(config.communication.mms.reports[0].triggerOptions.dataUpdate);
    REQUIRE_FALSE(config.communication.mms.reports[1].enabled);
}

TEST_CASE("runtime validation rejects inconsistent or unsafe configuration") {
    SECTION("JSON explicitly contains no turbines") {
        const sc::test::TemporaryCsv file("{\"turbines\": []}\n");
        REQUIRE_THROWS_AS(sc::runtime::loadRuntimeConfig(file.path()), std::runtime_error);
    }

    SECTION("empty turbine list") {
        auto config = sc::runtime::defaultRuntimeConfig();
        config.turbines.clear();
        REQUIRE_THROWS_AS(sc::runtime::validateRuntimeConfig(config), std::runtime_error);
    }

    SECTION("yaw LUT count mismatch") {
        const auto config = sc::runtime::defaultRuntimeConfig();
        REQUIRE_THROWS_AS(sc::runtime::validateRuntimeConfig(config, 8), std::runtime_error);
    }

    SECTION("duplicate server ports") {
        auto config = sc::runtime::defaultRuntimeConfig();
        config.communication.attackInterface.port = config.communication.operatorServer.port;
        REQUIRE_THROWS_AS(sc::runtime::validateRuntimeConfig(config), std::runtime_error);
    }

    SECTION("non-positive period") {
        auto config = sc::runtime::defaultRuntimeConfig();
        config.tasks.monitoringPeriod = 0ms;
        REQUIRE_THROWS_AS(sc::runtime::validateRuntimeConfig(config), std::runtime_error);
    }

    SECTION("unsupported schema version") {
        auto config = sc::runtime::defaultRuntimeConfig();
        config.schemaVersion = 2;
        REQUIRE_THROWS_AS(sc::runtime::validateRuntimeConfig(config), std::runtime_error);
    }

    SECTION("duplicate turbine IDs") {
        auto config = sc::runtime::defaultRuntimeConfig();
        config.turbines[1].id = config.turbines[0].id;
        REQUIRE_THROWS_AS(sc::runtime::validateRuntimeConfig(config), std::runtime_error);
    }

    SECTION("multiple enabled reports are reserved for a later implementation") {
        auto config = sc::runtime::defaultRuntimeConfig();
        config.communication.mms.reports.push_back({"diagnostics"});
        REQUIRE_THROWS_AS(sc::runtime::validateRuntimeConfig(config), std::runtime_error);
    }
}

TEST_CASE("global per-turbine state can be sized from runtime configuration") {
    GlobalData data;
    data.resizeForTurbines(4);

    REQUIRE(data.lastWS.size() == 4);
    REQUIRE(data.lastGenTorque_t.size() == 4);
    REQUIRE(data.wsHistory.size() == 4);
    REQUIRE(data.genTorqueHistory.size() == 4);
    REQUIRE(data.AvailablePower.size() == 4);
    REQUIRE(data.TurbinePowerSetpoints.size() == 4);
    REQUIRE(data.TurbineYawSetpoints.size() == 4);
    REQUIRE(data.enableTurbine.size() == 4);
    REQUIRE(data.TurbineController.size() == 4);
    REQUIRE(data.orientations.size() == 4);
    REQUIRE(data.TurbinePowerSetpoints[0] == -1.0F);
    REQUIRE(data.TurbineController[0] == GlobalData::turbineControllerKomega2);
}
