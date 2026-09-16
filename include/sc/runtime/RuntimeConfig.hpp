#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace sc::runtime {

struct TurbineEndpointConfig {
    std::string id;
    std::string host{"localhost"};
    int port{102};
    std::string iedName{"WTURBINE"};
    std::string logicalDevice{"LD0"};
};

struct ReportTriggerOptions {
    bool dataChange{true};
    bool qualityChange{true};
    bool dataUpdate{false};
    bool integrity{true};
    bool generalInterrogation{true};
};

struct ReportConfig {
    ReportConfig() = default;
    ReportConfig(std::string reportName) : name(std::move(reportName)) {}

    std::string name;
    bool enabled{true};
    std::chrono::milliseconds integrityPeriod{500};
    std::string dataSetReference{"WPPD1$ds01"};
    std::string controlBlockReference{"WPPD1$RP$urcb01"};
    std::vector<std::string> dataReferences;
    ReportTriggerOptions triggerOptions;
};

struct RuntimeConfig {
    int schemaVersion{1};
    std::vector<TurbineEndpointConfig> turbines;

    struct Tasks {
        std::chrono::milliseconds controlPeriod{4000};
        std::chrono::milliseconds signalProcessingPeriod{500};
        std::chrono::milliseconds monitoringPeriod{50};
    } tasks;

    struct Control {
        std::filesystem::path yawLutCsvPath{"yaw_lut.csv"};
    } control;

    struct Hmi {
        std::chrono::milliseconds period{500};
        int windowSize{300};
#ifdef _WIN32
        std::string publisherEndpoint{"tcp://*:5555"};
        std::string commandEndpoint{"tcp://*:5556"};
#else
        std::string publisherEndpoint{"ipc:///tmp/supervisory_controller_hmi.sock"};
        std::string commandEndpoint{"ipc:///tmp/supervisory_controller_hmi_cmd.sock"};
#endif
    } hmi;

    struct Historian {
        std::string experimentName{"project_datahistorian"};
        std::filesystem::path outputDirectory{"data"};
        std::size_t flushEvery{512};
        std::chrono::milliseconds flushPeriod{5000};
    } historian;

    struct SocketServer {
        int port;
        std::chrono::milliseconds pollPeriod{10};
    };

    struct Communication {
        SocketServer operatorServer{9001};
        SocketServer attackInterface{9002};
        SocketServer dataHistorian{9003};

        struct Mms {
            std::chrono::milliseconds pollPeriod{10};
            std::vector<ReportConfig> reports{{"operational"}};
        } mms;

        struct Goose {
            std::string networkInterface{"veth1"};
            std::chrono::milliseconds pollPeriod{4};
        } goose;

        std::chrono::milliseconds orchestrationPeriod{10};
    } communication;
};

RuntimeConfig defaultRuntimeConfig();
RuntimeConfig loadRuntimeConfig(const std::filesystem::path& jsonPath);
void validateRuntimeConfig(const RuntimeConfig& config);
void validateRuntimeConfig(const RuntimeConfig& config, std::size_t yawLutTurbineCount);

} // namespace sc::runtime
