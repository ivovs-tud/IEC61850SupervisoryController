#include "sc/runtime/RuntimeConfig.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sc::runtime {
namespace {

using boost::property_tree::ptree;

template <typename Duration>
void loadMilliseconds(const ptree& tree, const std::string& path, Duration& destination) {
    if (const auto value = tree.get_optional<long long>(path)) {
        destination = std::chrono::milliseconds(*value);
    }
}

void requirePositive(std::chrono::milliseconds value, const std::string& field) {
    if (value.count() <= 0) {
        throw std::runtime_error(field + " must be greater than zero");
    }
}

void validateServer(const RuntimeConfig::SocketServer& server, const std::string& name) {
    if (server.port < 1024 || server.port > 65535) {
        throw std::runtime_error(name + ".port must be between 1024 and 65535");
    }
    requirePositive(server.pollPeriod, name + ".poll_period_ms");
}

std::filesystem::path configuredPath(const std::filesystem::path& configPath,
                                     const std::string& value) {
    std::filesystem::path path(value);
    if (path.is_relative()) {
        path = configPath.parent_path() / path;
    }
    return path.lexically_normal();
}

} // namespace

RuntimeConfig defaultRuntimeConfig() {
    RuntimeConfig config;
    config.turbines.reserve(9);
    for (int port = 102; port <= 110; ++port) {
        config.turbines.push_back({
            "WT" + std::to_string(port - 101), "localhost", port, "WTURBINE", "LD0"});
    }
    return config;
}

RuntimeConfig loadRuntimeConfig(const std::filesystem::path& jsonPath) {
    RuntimeConfig config = defaultRuntimeConfig();
    ptree root;
    try {
        boost::property_tree::read_json(jsonPath.string(), root);
        config.schemaVersion = root.get<int>("schema_version", config.schemaVersion);

        if (const auto turbines = root.get_child_optional("turbines")) {
            config.turbines.clear();
            std::size_t turbineIndex = 0;
            for (const auto& entry : *turbines) {
                const auto& node = entry.second;
                const auto nestedMms = node.get_child_optional("mms");
                const auto& mms = nestedMms ? *nestedMms : node;
                TurbineEndpointConfig endpoint;
                endpoint.id = node.get<std::string>(
                    "id", "WT" + std::to_string(turbineIndex + 1));
                endpoint.host = mms.get<std::string>("host");
                endpoint.port = mms.get<int>("port");
                endpoint.iedName = mms.get<std::string>("ied_name", "WTURBINE");
                endpoint.logicalDevice = mms.get<std::string>("logical_device", "LD0");
                config.turbines.push_back(std::move(endpoint));
                ++turbineIndex;
            }
        }

        loadMilliseconds(root, "tasks.control_period_ms", config.tasks.controlPeriod);
        loadMilliseconds(root, "tasks.signal_processing_period_ms", config.tasks.signalProcessingPeriod);
        loadMilliseconds(root, "tasks.monitoring_period_ms", config.tasks.monitoringPeriod);

        if (const auto value = root.get_optional<std::string>("control.yaw_lut_csv")) {
            config.control.yawLutCsvPath = configuredPath(jsonPath, *value);
        }

        loadMilliseconds(root, "hmi.period_ms", config.hmi.period);
        config.hmi.windowSize = root.get<int>("hmi.window_size", config.hmi.windowSize);
        config.hmi.publisherEndpoint = root.get<std::string>(
            "hmi.publisher_endpoint", config.hmi.publisherEndpoint);
        config.hmi.commandEndpoint = root.get<std::string>(
            "hmi.command_endpoint", config.hmi.commandEndpoint);

        config.historian.experimentName = root.get<std::string>(
            "historian.experiment_name", config.historian.experimentName);
        if (const auto value = root.get_optional<std::string>("historian.output_directory")) {
            config.historian.outputDirectory = configuredPath(jsonPath, *value);
        }
        config.historian.flushEvery = root.get<std::size_t>(
            "historian.flush_every", config.historian.flushEvery);
        loadMilliseconds(root, "historian.flush_period_ms", config.historian.flushPeriod);

        config.communication.operatorServer.port = root.get<int>(
            "communication.operator.port", config.communication.operatorServer.port);
        loadMilliseconds(root, "communication.operator.poll_period_ms",
                         config.communication.operatorServer.pollPeriod);
        config.communication.attackInterface.port = root.get<int>(
            "communication.attack_interface.port", config.communication.attackInterface.port);
        loadMilliseconds(root, "communication.attack_interface.poll_period_ms",
                         config.communication.attackInterface.pollPeriod);
        config.communication.dataHistorian.port = root.get<int>(
            "communication.data_historian.port", config.communication.dataHistorian.port);
        loadMilliseconds(root, "communication.data_historian.poll_period_ms",
                         config.communication.dataHistorian.pollPeriod);
        loadMilliseconds(root, "communication.orchestration_period_ms",
                         config.communication.orchestrationPeriod);

        loadMilliseconds(root, "communication.mms.poll_period_ms",
                         config.communication.mms.pollPeriod);
        if (const auto reports = root.get_child_optional("communication.mms.reports")) {
            config.communication.mms.reports.clear();
            std::size_t reportIndex = 0;
            for (const auto& entry : *reports) {
                const auto& node = entry.second;
                ReportConfig report;
                report.name = node.get<std::string>(
                    "name", "report-" + std::to_string(reportIndex + 1));
                report.enabled = node.get<bool>("enabled", report.enabled);
                loadMilliseconds(node, "integrity_period_ms", report.integrityPeriod);
                report.dataSetReference = node.get<std::string>(
                    "dataset_reference", report.dataSetReference);
                report.controlBlockReference = node.get<std::string>(
                    "control_block_reference", report.controlBlockReference);
                if (const auto references = node.get_child_optional("data_references")) {
                    report.dataReferences.clear();
                    for (const auto& reference : *references) {
                        report.dataReferences.push_back(
                            reference.second.get_value<std::string>());
                    }
                }
                report.triggerOptions.dataChange = node.get<bool>(
                    "trigger_options.data_change", report.triggerOptions.dataChange);
                report.triggerOptions.qualityChange = node.get<bool>(
                    "trigger_options.quality_change", report.triggerOptions.qualityChange);
                report.triggerOptions.dataUpdate = node.get<bool>(
                    "trigger_options.data_update", report.triggerOptions.dataUpdate);
                report.triggerOptions.integrity = node.get<bool>(
                    "trigger_options.integrity", report.triggerOptions.integrity);
                report.triggerOptions.generalInterrogation = node.get<bool>(
                    "trigger_options.general_interrogation",
                    report.triggerOptions.generalInterrogation);
                config.communication.mms.reports.push_back(std::move(report));
                ++reportIndex;
            }
        } else {
            // Version-1 compatibility for the original single-report fields.
            auto& report = config.communication.mms.reports.front();
            report.enabled = root.get<bool>(
                "communication.mms.reporting_enabled", report.enabled);
            loadMilliseconds(root, "communication.mms.report_trigger_period_ms",
                             report.integrityPeriod);
            report.dataSetReference = root.get<std::string>(
                "communication.mms.report_dataset_reference", report.dataSetReference);
            report.controlBlockReference = root.get<std::string>(
                "communication.mms.report_control_block_reference",
                report.controlBlockReference);
            if (const auto references = root.get_child_optional(
                    "communication.mms.report_data_references")) {
                report.dataReferences.clear();
                for (const auto& entry : *references) {
                    report.dataReferences.push_back(entry.second.get_value<std::string>());
                }
            }
        }

        config.communication.goose.networkInterface = root.get<std::string>(
            "communication.goose.network_interface",
            config.communication.goose.networkInterface);
        loadMilliseconds(root, "communication.goose.poll_period_ms",
                         config.communication.goose.pollPeriod);
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "Failed to load runtime configuration '" + jsonPath.string() + "': " + error.what());
    }

    validateRuntimeConfig(config);
    return config;
}

void validateRuntimeConfig(const RuntimeConfig& config) {
    if (config.schemaVersion != 1) {
        throw std::runtime_error(
            "unsupported schema_version " + std::to_string(config.schemaVersion));
    }
    if (config.turbines.empty()) {
        throw std::runtime_error("turbines must contain at least one endpoint");
    }
    if (config.turbines.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("turbines contains too many endpoints");
    }
    std::set<std::string> turbineIds;
    for (std::size_t index = 0; index < config.turbines.size(); ++index) {
        const auto& endpoint = config.turbines[index];
        const std::string prefix = "turbines[" + std::to_string(index) + "]";
        if (endpoint.id.empty()) {
            throw std::runtime_error(prefix + ".id must not be empty");
        }
        if (!turbineIds.insert(endpoint.id).second) {
            throw std::runtime_error(prefix + ".id must be unique");
        }
        if (endpoint.host.empty()) {
            throw std::runtime_error(prefix + ".host must not be empty");
        }
        if (endpoint.port < 1 || endpoint.port > 65535) {
            throw std::runtime_error(prefix + ".port must be between 1 and 65535");
        }
        if (endpoint.iedName.empty()) {
            throw std::runtime_error(prefix + ".ied_name must not be empty");
        }
        if (endpoint.logicalDevice.empty()) {
            throw std::runtime_error(prefix + ".logical_device must not be empty");
        }
    }

    requirePositive(config.tasks.controlPeriod, "tasks.control_period_ms");
    requirePositive(config.tasks.signalProcessingPeriod, "tasks.signal_processing_period_ms");
    requirePositive(config.tasks.monitoringPeriod, "tasks.monitoring_period_ms");
    if (config.control.yawLutCsvPath.empty()) {
        throw std::runtime_error("control.yaw_lut_csv must not be empty");
    }

    requirePositive(config.hmi.period, "hmi.period_ms");
    if (config.hmi.windowSize <= 0) {
        throw std::runtime_error("hmi.window_size must be greater than zero");
    }
    if (config.hmi.publisherEndpoint.empty() || config.hmi.commandEndpoint.empty()) {
        throw std::runtime_error("HMI endpoints must not be empty");
    }

    if (config.historian.experimentName.empty()) {
        throw std::runtime_error("historian.experiment_name must not be empty");
    }
    if (config.historian.outputDirectory.empty()) {
        throw std::runtime_error("historian.output_directory must not be empty");
    }
    if (config.historian.flushEvery == 0) {
        throw std::runtime_error("historian.flush_every must be greater than zero");
    }
    requirePositive(config.historian.flushPeriod, "historian.flush_period_ms");

    validateServer(config.communication.operatorServer, "communication.operator");
    validateServer(config.communication.attackInterface, "communication.attack_interface");
    validateServer(config.communication.dataHistorian, "communication.data_historian");
    const std::set<int> serverPorts{
        config.communication.operatorServer.port,
        config.communication.attackInterface.port,
        config.communication.dataHistorian.port,
    };
    if (serverPorts.size() != 3) {
        throw std::runtime_error("communication server ports must be distinct");
    }

    requirePositive(config.communication.orchestrationPeriod,
                    "communication.orchestration_period_ms");
    requirePositive(config.communication.mms.pollPeriod,
                    "communication.mms.poll_period_ms");
    std::set<std::string> reportNames;
    std::size_t enabledReportCount = 0;
    for (std::size_t index = 0; index < config.communication.mms.reports.size(); ++index) {
        const auto& report = config.communication.mms.reports[index];
        const std::string prefix =
            "communication.mms.reports[" + std::to_string(index) + "]";
        if (report.name.empty()) {
            throw std::runtime_error(prefix + ".name must not be empty");
        }
        if (!reportNames.insert(report.name).second) {
            throw std::runtime_error(prefix + ".name must be unique");
        }
        requirePositive(report.integrityPeriod, prefix + ".integrity_period_ms");
        for (const auto& reference : report.dataReferences) {
            if (reference.empty()) {
                throw std::runtime_error(prefix + ".data_references must not contain empty values");
            }
        }
        if (report.enabled) {
            ++enabledReportCount;
            if (report.dataSetReference.empty() || report.controlBlockReference.empty()) {
                throw std::runtime_error(
                    prefix + " requires dataset and control-block references when enabled");
            }
        }
    }
    if (enabledReportCount > 1) {
        throw std::runtime_error(
            "the current IEC communicator supports at most one enabled MMS report; "
            "additional report definitions must remain disabled");
    }
    if (config.communication.goose.networkInterface.empty()) {
        throw std::runtime_error("communication.goose.network_interface must not be empty");
    }
    requirePositive(config.communication.goose.pollPeriod,
                    "communication.goose.poll_period_ms");
}

void validateRuntimeConfig(const RuntimeConfig& config, std::size_t yawLutTurbineCount) {
    validateRuntimeConfig(config);
    if (yawLutTurbineCount != config.turbines.size()) {
        std::ostringstream message;
        message << "yaw LUT defines " << yawLutTurbineCount
                << " turbine columns, but runtime configuration defines "
                << config.turbines.size() << " turbine endpoints";
        throw std::runtime_error(message.str());
    }
}

} // namespace sc::runtime
