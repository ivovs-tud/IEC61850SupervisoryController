#include <chrono>
#include <exception>
#include <iostream>

using namespace std::chrono_literals;

#include "common/config.hpp"
#include "common/ConsoleColors.hpp"
#include "tasks/HmiTask.hpp"
#include "tasks/ControlTask.hpp"
#include "tasks/SignalProcessingTask.hpp"
#include "tasks/MonitoringTask.hpp"
#include "communication/CommunicationTask.hpp"
#include "common/DataHistorian.hpp"

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm")
#endif
int main(int argc, char* argv[])
{
    // Initialize console colors for Windows (enables ANSI escape codes)
    enableWindowsConsoleColors();
    
#ifdef _WIN32
    // Increase timer resolution on Windows so short sleeps are accurate.
    // This requests a 1 ms timer resolution for the process; it's restored
    // automatically when the guard object is destroyed.
    struct WinTimerResolutionGuard {
        WinTimerResolutionGuard() { timeBeginPeriod(1); }
        ~WinTimerResolutionGuard() { timeEndPeriod(1); }
    } _winTimerGuard;
    SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
#endif
    
    char yaw_str[256];
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <yaw_lut.csv>\n";
		strcpy(yaw_str, "yaw_lut.csv");
        //return 1;
    } else {
		strcpy(yaw_str, argv[1]);
    }

    try {
        const int numTurbines = 12;

        // TODO: drop privileges (e.g. setuid/setgid) before spawning worker threads

        HmiTask hmiTask(defaultHmiConfig(numTurbines), 500ms);  // 10 Hz
              // 0.25 Hz
        SignalProcessingTask signalTask(500ms);      // 1 kHz
        MonitoringTask       monitoringTask(50ms); // 20 Hz

        // All fields have sensible defaults – override only what you need.
        CommConfig cfg;
        // cfg.operatorServer.port       = 9001;
        // cfg.operatorServer.pollPeriod = 10ms;
        // cfg.attackInterface.port      = 9002;
        // cfg.attackInterface.pollPeriod = 10ms;
        // cfg.dataHistorian.port        = 9003;
        // cfg.dataHistorian.pollPeriod  = 10ms;
        cfg.mms.turbines = {
            {"localhost", 102, "WTURBINE", "LD0"},
            {"localhost", 103, "WTURBINE", "LD0"},
            {"localhost", 104, "WTURBINE", "LD0"},
            // {"localhost", 105, "WTURBINE", "LD0"},
            // {"localhost", 106, "WTURBINE", "LD0"},
            // {"localhost", 107, "WTURBINE", "LD0"},
            // {"localhost", 108, "WTURBINE", "LD0"},
            // {"localhost", 109, "WTURBINE", "LD0"},
            // {"localhost", 110, "WTURBINE", "LD0"},
            // {"localhost", 111, "WTURBINE", "LD0"},
        };
        // cfg.goose.networkIface        = "eth1";
        cfg.orchestrationPeriod = 10ms;
        DataHistorian::instance().configure("project_datahistorian");
        DataHistorian::instance().start();
        CommunicationTask commTask(cfg);
        commTask.init();

        ControlTask::Config controlConfig;
        controlConfig.period = 4000ms;
        controlConfig.yawLutCsvPath = yaw_str;
        controlConfig.numTurbines = numTurbines;
        ControlTask controlTask(controlConfig);

        hmiTask.start();
        controlTask.start();
        signalTask.start();
        monitoringTask.start();
        commTask.start();

        std::cout << "Server running. Press Enter to stop.\n";

        std::cin.get();

        hmiTask.stop();
        controlTask.stop();
        signalTask.stop();
        // monitoringTask.stop();
        commTask.stop();

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Failed to start supervisory controller: " << ex.what() << '\n';
        return 1;
    }
}
