#include <catch2/catch_test_macros.hpp>

#include <boost/property_tree/json_parser.hpp>

#include <chrono>
#include <filesystem>
#include <string>

#include "sc/model/SharedData.hpp"
#include "sc/application/YawLut.hpp"
#include "sc/communication/attack/AttackProtocol.hpp"
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
    REQUIRE_FALSE(config.monitoring.alarmAcknowledgementEnabled);
    REQUIRE(config.communication.attackInterface.transport == sc::ports::AttackTransport::ZEROMQ);
    REQUIRE(config.communication.attackInterface.bindAddress == "0.0.0.0");
    REQUIRE(config.communication.attackInterface.heartbeatInterval == 200ms);
    REQUIRE(config.communication.attackInterface.leaseTimeout == 750ms);
    REQUIRE(config.communication.attackInterface.reuseLastFdiValueOnFailure);
    REQUIRE(config.communication.attackInterface.receiveBufferBytes == 64 * 1024);
    REQUIRE(config.communication.attackInterface.transmitBufferBytes == 64 * 1024);
    REQUIRE(config.communication.attackInterface.configurationTimeout == 1000ms);
    REQUIRE(config.communication.mms.reconnectInitialDelay == 100ms);
    REQUIRE(config.communication.mms.reconnectMaxDelay == 5000ms);
    REQUIRE_FALSE(config.communication.goose.enabled);
    REQUIRE_NOTHROW(sc::runtime::validateRuntimeConfig(config, 9));
}

TEST_CASE("default runtime turbine list matches the shipped yaw LUT") {
    const auto config = sc::runtime::defaultRuntimeConfig();
    const sc::application::YawLut yawLut(
        (std::filesystem::path(SC_SOURCE_DIR) / "config/yaw_lut.csv").string());

    REQUIRE_NOTHROW(sc::runtime::validateRuntimeConfig(config, yawLut.turbineCount()));
}

TEST_CASE("shipped versioned JSON example loads and matches its yaw LUT") {
    const auto config = sc::runtime::loadRuntimeConfig(
        std::filesystem::path(SC_SOURCE_DIR) / "config/default.json");
    const sc::application::YawLut yawLut(config.control.yawLutCsvPath.string());

    REQUIRE(config.turbines.size() == 9);
    REQUIRE(config.communication.mms.reports.size() == 2);
    REQUIRE(config.communication.mms.reports[0].enabled);
    REQUIRE_FALSE(config.communication.mms.reports[1].enabled);
    REQUIRE_NOTHROW(sc::runtime::validateRuntimeConfig(config, yawLut.turbineCount()));
}

TEST_CASE("Windows local configuration binds the HMI to loopback") {
    const auto config = sc::runtime::loadRuntimeConfig(
        std::filesystem::path(SC_SOURCE_DIR) / "config/windows-local.json");

    REQUIRE(config.hmi.publisherEndpoint == "tcp://127.0.0.1:5555");
    REQUIRE(config.hmi.commandEndpoint == "tcp://127.0.0.1:5556");
}

TEST_CASE("runtime schema limits match runtime validation boundaries") {
    boost::property_tree::ptree schema;
    boost::property_tree::read_json(
        (std::filesystem::path(SC_SOURCE_DIR) / "config/runtime-config.schema.json").string(),
        schema);

    REQUIRE(schema.get<int>("properties.schema_version.const") ==
            sc::runtime::defaultRuntimeConfig().schemaVersion);
    REQUIRE(schema.get<int>("properties.turbines.minItems") == 1);
    REQUIRE(schema.get<int>("properties.turbines.maxItems") == 255);
    REQUIRE(schema.get<int>("$defs.socketServer.properties.port.minimum") == 1024);
    REQUIRE(schema.get<int>("$defs.socketServer.properties.port.maximum") == 65535);
    REQUIRE(schema.get<int>("$defs.attackInterfaceServer.properties.receive_buffer_bytes.minimum") ==
            static_cast<int>(sc::protocol::attack::CFG_DATA_SIZE));
    REQUIRE(schema.get<int>("$defs.attackInterfaceServer.properties.transmit_buffer_bytes.minimum") ==
            static_cast<int>(sc::protocol::attack::CFG_DATA_SIZE));

    auto config = sc::runtime::defaultRuntimeConfig();
    config.communication.operatorServer.port = 1024;
    config.communication.attackInterface.receiveBufferBytes =
        sc::protocol::attack::CFG_DATA_SIZE;
    config.communication.attackInterface.transmitBufferBytes =
        sc::protocol::attack::CFG_DATA_SIZE;
    REQUIRE_NOTHROW(sc::runtime::validateRuntimeConfig(config));

    config.communication.operatorServer.port = 65536;
    REQUIRE_THROWS_AS(sc::runtime::validateRuntimeConfig(config), std::runtime_error);
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
        "  \"monitoring\": {\"alarm_acknowledgement_enabled\": true},\n"
        "  \"hmi\": {\"window_size\": 42},\n"
        "  \"historian\": {\"output_directory\": \"logs\"},\n"
        "  \"communication\": {\n"
        "    \"attack_interface\": {\n"
        "      \"transport\": \"tcp\", \"bind_address\": \"127.0.0.1\",\n"
        "      \"reuse_last_fdi_value_on_failure\": false,\n"
        "      \"receive_buffer_bytes\": 4096, \"transmit_buffer_bytes\": 8192,\n"
        "      \"configuration_timeout_ms\": 900,\n"
        "      \"tcp_user_timeout_ms\": 1200\n"
        "    },\n"
        "    \"mms\": {\"reconnect_initial_delay_ms\": 25, \"reconnect_max_delay_ms\": 250, \"reporting_enabled\": false},\n"
        "    \"goose\": {\"enabled\": true, \"network_interface\": \"test0\"}\n"
        "  }\n"
        "}\n");

    const auto config = sc::runtime::loadRuntimeConfig(file.path());
    const std::filesystem::path parent = std::filesystem::path(file.path()).parent_path();

    REQUIRE(config.turbines.size() == 2);
    REQUIRE(config.turbines[0].host == "10.0.0.1");
    REQUIRE(config.turbines[0].iedName == "A");
    REQUIRE(config.turbines[1].iedName == "WTURBINE");
    REQUIRE(config.tasks.controlPeriod == 25ms);
    REQUIRE(config.hmi.windowSize == 42);
    REQUIRE(config.monitoring.alarmAcknowledgementEnabled);
    REQUIRE(config.control.yawLutCsvPath == parent / "lut.csv");
    REQUIRE(config.historian.outputDirectory == parent / "logs");
    REQUIRE(config.communication.attackInterface.transport == sc::ports::AttackTransport::TCP);
    REQUIRE(config.communication.attackInterface.bindAddress == "127.0.0.1");
    REQUIRE_FALSE(config.communication.attackInterface.reuseLastFdiValueOnFailure);
    REQUIRE(config.communication.attackInterface.receiveBufferBytes == 4096);
    REQUIRE(config.communication.attackInterface.transmitBufferBytes == 8192);
    REQUIRE(config.communication.attackInterface.configurationTimeout == 900ms);
    REQUIRE(config.communication.attackInterface.tcpUserTimeout == 1200ms);
    REQUIRE(config.communication.mms.reconnectInitialDelay == 25ms);
    REQUIRE(config.communication.mms.reconnectMaxDelay == 250ms);
    REQUIRE_FALSE(config.communication.mms.reports.front().enabled);
    REQUIRE(config.communication.goose.enabled);
    REQUIRE(config.communication.goose.networkInterface == "test0");
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
        "      \"data_references\": [\"WTUR1$MX$W\"]\n"
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

    SECTION("attack protocol limits the turbine count to uint8 identifiers") {
        auto config = sc::runtime::defaultRuntimeConfig();
        config.turbines.resize(256, config.turbines.front());
        for (std::size_t index = 0; index < config.turbines.size(); ++index) {
            config.turbines[index].id = "WT" + std::to_string(index + 1);
        }
        REQUIRE_THROWS_AS(sc::runtime::validateRuntimeConfig(config), std::runtime_error);
    }

    SECTION("controller servers cannot use privileged ports") {
        auto config = sc::runtime::defaultRuntimeConfig();
        config.communication.operatorServer.port = 1023;
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

    SECTION("attack lease must exceed its heartbeat interval") {
        auto config = sc::runtime::defaultRuntimeConfig();
        config.communication.attackInterface.leaseTimeout = config.communication.attackInterface.heartbeatInterval;
        REQUIRE_THROWS_AS(sc::runtime::validateRuntimeConfig(config), std::runtime_error);
    }

    SECTION("attack buffers must fit the configuration message") {
        auto config = sc::runtime::defaultRuntimeConfig();
        config.communication.attackInterface.receiveBufferBytes = 267;
        REQUIRE_THROWS_AS(sc::runtime::validateRuntimeConfig(config), std::runtime_error);
    }

    SECTION("attack configuration timeout must be positive") {
        auto config = sc::runtime::defaultRuntimeConfig();
        config.communication.attackInterface.configurationTimeout = 0ms;
        REQUIRE_THROWS_AS(sc::runtime::validateRuntimeConfig(config), std::runtime_error);
    }

    SECTION("raw TCP bind address must be an IPv4 address") {
        auto config = sc::runtime::defaultRuntimeConfig();
        config.communication.attackInterface.transport = sc::ports::AttackTransport::TCP;
        config.communication.attackInterface.bindAddress = "localhost";
        REQUIRE_THROWS_AS(sc::runtime::validateRuntimeConfig(config), std::runtime_error);
    }

    SECTION("unknown attack transport") {
        const sc::test::TemporaryCsv file(
            "{\"communication\":{\"attack_interface\":{\"transport\":\"udp\"}}}\n");
        REQUIRE_THROWS_AS(sc::runtime::loadRuntimeConfig(file.path()), std::runtime_error);
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

    SECTION("MMS reconnect maximum must not be shorter than the initial delay") {
        auto config = sc::runtime::defaultRuntimeConfig();
        config.communication.mms.reconnectInitialDelay = 500ms;
        config.communication.mms.reconnectMaxDelay = 100ms;
        REQUIRE_THROWS_AS(sc::runtime::validateRuntimeConfig(config), std::runtime_error);
    }

    SECTION("enabled GOOSE requires a network interface") {
        auto config = sc::runtime::defaultRuntimeConfig();
        config.communication.goose.enabled = true;
        config.communication.goose.networkInterface.clear();
        REQUIRE_THROWS_AS(sc::runtime::validateRuntimeConfig(config), std::runtime_error);
    }
}

TEST_CASE("shared task data can be sized from runtime configuration") {
    SharedData data;
    data.configureTurbineCount(4);

    REQUIRE(data.collected.lastWS.size() == 4);
    REQUIRE(data.collected.lastGenTorque_t.size() == 4);
    REQUIRE(data.collected.wsHistory.size() == 4);
    REQUIRE(data.collected.genTorqueHistory.size() == 4);
    REQUIRE(data.processed.availablePower.size() == 4);
    REQUIRE(data.processed.measuredTotalPowerHistory.capacity() == CollectedData::historySampleCapacity);
    REQUIRE(data.control.powerSetpoints.size() == 4);
    REQUIRE(data.control.yawSetpoints.size() == 4);
    REQUIRE(data.control.turbineEnabled.size() == 4);
    REQUIRE(data.control.turbineController.size() == 4);
    REQUIRE(data.control.powerSetpoints[0] == -1.0F);
    REQUIRE(data.control.turbineController[0] == ControlData::controllerKomega2);
}

TEST_CASE("shared task data sections have independent mutexes") {
    SharedData data;
    std::lock_guard<std::mutex> collectedLock(data.collected.mutex);
    std::unique_lock<std::mutex> processedLock(data.processed.mutex, std::try_to_lock);
    std::unique_lock<std::mutex> controlLock(data.control.mutex, std::try_to_lock);
    std::unique_lock<std::mutex> monitoringLock(data.monitoring.mutex, std::try_to_lock);
    std::unique_lock<std::mutex> interfaceLock(data.interface.mutex, std::try_to_lock);

    REQUIRE(processedLock.owns_lock());
    REQUIRE(controlLock.owns_lock());
    REQUIRE(monitoringLock.owns_lock());
    REQUIRE(interfaceLock.owns_lock());
}
