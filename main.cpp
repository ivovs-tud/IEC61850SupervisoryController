#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

using namespace std::chrono_literals;

#include "common/ConsoleColors.hpp"
#include "common/DataHistorian.hpp"
#include "common/GlobalDataStructure.hpp"
#include "common/config.hpp"
#include "communication/CommunicationTask.hpp"
#include "sc/application/YawLut.hpp"
#include "sc/runtime/RuntimeConfig.hpp"
#include "tasks/ControlTask.hpp"
#include "tasks/HmiTask.hpp"
#include "tasks/MonitoringTask.hpp"
#include "tasks/SignalProcessingTask.hpp"

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm")
#endif

namespace {

struct CliOptions {
    std::optional<std::filesystem::path> configPath;
    std::optional<std::filesystem::path> yawLutOverride;
    bool showHelp{false};
};

CliOptions parseArguments(int argc, char* argv[]) {
    CliOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            options.showHelp = true;
        } else if (argument == "--config") {
            if (++i >= argc) {
                throw std::runtime_error("--config requires a JSON file path");
            }
            options.configPath = argv[i];
        } else if (argument == "--yaw-lut") {
            if (++i >= argc) {
                throw std::runtime_error("--yaw-lut requires a CSV file path");
            }
            options.yawLutOverride = argv[i];
        } else if (!argument.empty() && argument.front() == '-') {
            throw std::runtime_error("Unknown option: " + argument);
        } else if (!options.yawLutOverride) {
            // Preserve the original `supervisory_controller yaw_lut.csv` form.
            options.yawLutOverride = argument;
        } else {
            throw std::runtime_error("Unexpected positional argument: " + argument);
        }
    }
    return options;
}

void printUsage(const char* executable) {
    std::cout << "Usage: " << executable
              << " [--config runtime.json] [--yaw-lut yaw_lut.csv]\n"
              << "       " << executable << " [yaw_lut.csv]\n";
}

CommConfig makeCommunicationConfig(const sc::runtime::RuntimeConfig& runtime) {
    CommConfig config;
    config.operatorServer.port = runtime.communication.operatorServer.port;
    config.operatorServer.pollPeriod = runtime.communication.operatorServer.pollPeriod;
    config.attackInterface.port = runtime.communication.attackInterface.port;
    config.attackInterface.pollPeriod = runtime.communication.attackInterface.pollPeriod;
    config.dataHistorian.port = runtime.communication.dataHistorian.port;
    config.dataHistorian.pollPeriod = runtime.communication.dataHistorian.pollPeriod;

    config.mms.turbines.reserve(runtime.turbines.size());
    for (const auto& endpoint : runtime.turbines) {
        config.mms.turbines.push_back(
            {endpoint.host, endpoint.port, endpoint.iedName, endpoint.logicalDevice});
    }
    config.mms.pollPeriod = runtime.communication.mms.pollPeriod;
    config.mms.reportingEnabled = false;
    for (const auto& report : runtime.communication.mms.reports) {
        if (!report.enabled) {
            continue;
        }
        config.mms.reportingEnabled = true;
        config.mms.reportTriggerPeriod = report.integrityPeriod;
        config.mms.reportDataSetReference = report.dataSetReference;
        config.mms.reportControlBlockReference = report.controlBlockReference;
        config.mms.reportDataReferences = report.dataReferences;
        break;
    }
    config.goose.networkInterface = runtime.communication.goose.networkInterface;
    config.goose.pollPeriod = runtime.communication.goose.pollPeriod;
    config.orchestrationPeriod = runtime.communication.orchestrationPeriod;
    return config;
}

} // namespace

int main(int argc, char* argv[]) {
    enableWindowsConsoleColors();

#ifdef _WIN32
    struct WinTimerResolutionGuard {
        WinTimerResolutionGuard() { timeBeginPeriod(1); }
        ~WinTimerResolutionGuard() { timeEndPeriod(1); }
    } winTimerResolutionGuard;
    SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
#endif

    try {
        const CliOptions options = parseArguments(argc, argv);
        if (options.showHelp) {
            printUsage(argv[0]);
            return 0;
        }

        sc::runtime::RuntimeConfig runtime = options.configPath
            ? sc::runtime::loadRuntimeConfig(*options.configPath)
            : sc::runtime::defaultRuntimeConfig();
        if (options.yawLutOverride) {
            runtime.control.yawLutCsvPath = *options.yawLutOverride;
        }

        sc::runtime::validateRuntimeConfig(runtime);
        const sc::application::YawLut yawLut(runtime.control.yawLutCsvPath.string());
        sc::runtime::validateRuntimeConfig(runtime, yawLut.turbineCount());

        const int numTurbines = static_cast<int>(runtime.turbines.size());
        GlobalDataStructure::instance().configureTurbineCount(runtime.turbines.size());

        HmiConfig hmiConfig = defaultHmiConfig(numTurbines);
        hmiConfig.windowSize = runtime.hmi.windowSize;
        hmiConfig.publisherEndpoint = runtime.hmi.publisherEndpoint;
        hmiConfig.commandEndpoint = runtime.hmi.commandEndpoint;
        if (hmiConfig.numTurbines != numTurbines) {
            throw std::runtime_error("HMI turbine count does not match runtime turbine endpoints");
        }

        ControlTask::Config controlConfig;
        controlConfig.period = runtime.tasks.controlPeriod;
        controlConfig.yawLutCsvPath = runtime.control.yawLutCsvPath.string();
        controlConfig.numTurbines = numTurbines;

        const CommConfig communicationConfig = makeCommunicationConfig(runtime);

        // All validation and dynamic state sizing is complete before any worker starts.
        HmiTask hmiTask(std::move(hmiConfig), runtime.hmi.period);
        ControlTask controlTask(controlConfig);
        SignalProcessingTask signalTask(runtime.tasks.signalProcessingPeriod);
        MonitoringTask monitoringTask(runtime.tasks.monitoringPeriod, numTurbines);
        CommunicationOrchestrator commTask(communicationConfig);
        commTask.init();

        DataHistorian::instance().configure(
            runtime.historian.experimentName,
            runtime.historian.outputDirectory,
            runtime.historian.flushEvery,
            runtime.historian.flushPeriod);
        DataHistorian::instance().start();

        hmiTask.start();
        controlTask.start();
        signalTask.start();
        monitoringTask.start();
        commTask.start();

        std::cout << "SCADA system running with " << numTurbines << " turbines. Press Enter to stop.\n";

        std::string ignoredLine;
        std::getline(std::cin, ignoredLine);
        std::cout << "Stop requested. Shutting down...\n";

        auto stopStep = [](const char* name, auto&& stopFn) {
            std::cout << "  stopping " << name << "..." << std::flush;
            stopFn();
            std::cout << " done\n";
        };

        hmiTask.requestStop();
        controlTask.requestStop();
        signalTask.requestStop();
        monitoringTask.requestStop();

        stopStep("HMI", [&]() { hmiTask.waitStopped(); });
        stopStep("control", [&]() { controlTask.waitStopped(); });
        stopStep("signal processing", [&]() { signalTask.waitStopped(); });
        stopStep("monitoring", [&]() { monitoringTask.waitStopped(); });
        stopStep("communication", [&]() { commTask.stop(); });
        stopStep("data historian", [&]() { DataHistorian::instance().stopRun(); });

        std::cout << "Shutdown complete.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Failed to start supervisory controller: " << ex.what() << '\n';
        printUsage(argv[0]);
        return 1;
    }
}
