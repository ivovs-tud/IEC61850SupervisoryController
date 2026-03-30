#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

#include "tasks/HmiTask.hpp"
#include "tasks/ControlTask.hpp"
#include "tasks/SignalProcessingTask.hpp"
#include "tasks/MonitoringTask.hpp"
#include "communication/CommunicationTask.hpp"

int main()
{
    const int numTurbines = 3; 

    // TODO: drop privileges (e.g. setuid/setgid) before spawning worker threads

    // HmiTask              hmiTask(100ms);      // 10 Hz
    ControlTask          controlTask(numTurbines, 4000ms);      // 0.25 Hz
    // SignalProcessingTask signalTask(1ms);      // 1 kHz
    // MonitoringTask       monitoringTask(50ms); // 20 Hz

    // All fields have sensible defaults – override only what you need.
    CommConfig cfg;
    // cfg.operatorServer.port       = 9001;
    // cfg.attackInterface.port      = 9002;
    cfg.mms.turbines = {
        {"localhost", 102, "WTURBINE", "LD0"},
        {"localhost", 102, "WTURBINE", "LD2"},
        {"localhost", 102, "WTURBINE", "LD3"},
    };
    // cfg.goose.networkIface        = "eth1";
    cfg.orchestrationPeriod       = 1000ms;
    CommunicationTask commTask(cfg);
    commTask.init();

    // hmiTask.start();
    controlTask.start();
    // signalTask.start();
    // monitoringTask.start();
    commTask.start();

    std::cout << "Server running. Press Enter to stop.\n";

    std::cin.get();

    // hmiTask.stop();
    controlTask.stop();
    // signalTask.stop();
    // monitoringTask.stop();
    commTask.stop();

    return 0;
}
