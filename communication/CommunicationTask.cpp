#include "CommunicationTask.hpp"
#include "socket/SocketWrapper.hpp"

CommunicationTask::CommunicationTask(const CommConfig& config)
    : PeriodicTask(config.orchestrationPeriod)
    , config_(config)
{
    // TODO: construct libiec_wrapper and SocketWrapper instances

    state.iec_status.store(COMM_DISCONNECTED);
    state.socket_status.store(COMM_DISCONNECTED);
    state.lastActivityTime = std::chrono::system_clock::now();
}

void CommunicationTask::init()
{
    // Initialise IEC 61850 connections for all turbines listed in CommConfig.
    if (iecWrapper_.init(config_.mms.turbines, config_.goose.networkInterface) != IEC_OK)
    {
        std::cerr << "[CommunicationTask] IEC61850 init failed – "
                     "check CommConfig::mms.turbines\n";
    }

    socketWrapper.AttachServerCallback([](const std::vector<float>& data) {
        if (data.size() > 0) {
            std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
            GlobalDataStructure::instance().data().RequestedReferencePower = data[0];
            std::cout << "Updated RequestedReferencePower to " << data[0] << "\n";
        }
    });

    // TODO: set up GOOSE subscriber via libiec_wrapper and register onGooseMessage() as callback
}

void CommunicationTask::onStart()
{
    state.socket_status.store(COMM_CONNECTING);
    socketWrapper.StartOperatorServer(config_.operatorServer.port);
    socketWrapper.StartAttackInterfaceServer(config_.attackInterface.port);
    state.socket_status.store(COMM_CONNECTED);

    state.iec_status.store(COMM_CONNECTING);
    iecWrapper_.start();
    state.iec_status.store(COMM_CONNECTED);

    
    iecWrapper_.printTurbineDataModel(1, 1000);  // print first 10 DA references of turbine 1 for sanity check

    auto support = iecWrapper_.checkTurbineSupport(1, IEC_STRINGS::REQ_REFS);
    for (auto& [ref, ok] : support)
        std::cout << ref << ": " << (ok ? "OK" : "NOT SUPPORTED") << "\n";

    support = iecWrapper_.checkTurbineSupport(1, IEC_STRINGS::REQ_CMDS);
    for (auto& [ref, ok] : support)
        std::cout << ref << ": " << (ok ? "OK" : "NOT SUPPORTED") << "\n";

    
}

void CommunicationTask::execute()
{
    // TODO: poll IEC 61850 + socket status
    for (int i = 0; i < static_cast<int>(config_.mms.turbines.size()); ++i) {
        if (i > 0) continue;
        float power_sp = GlobalDataStructure::instance().data().TurbinePowerSetpoints[i];
        int yaw_sp = GlobalDataStructure::instance().data().TurbineYawSetpoints[i];
        // if (power_sp >= 0.0f || yaw_sp >= 0) {
        if (iecWrapper_.txSetpoint(i + 1, power_sp, yaw_sp) != IEC_OK) {
            std::cerr << "[CommunicationTask] Failed to send setpoint to turbine "
                        << (i + 1) << "\n";
        } else {
            std::cout << "Sent setpoint to turbine " << (i + 1)
                        << ": power=" << power_sp << " W, yaw=" << yaw_sp << " deg\n";
        }

        std::string secret;
        if (iecWrapper_.rxSecret(i + 1, secret) == IEC_OK) {
            std::cout << "Received secret from turbine " << (i + 1) << ": " << secret << "\n";
        } else {
            std::cerr << "[CommunicationTask] Failed to read secret from turbine "
                        << (i + 1) << "\n";
        }

        // Read Wind Speed Measurement From Each Turbine
        float windSpeed;
        if (iecWrapper_.rxWindSpeed(i + 1, windSpeed) == IEC_OK) {
            std::cout << "Received wind speed from turbine " << (i + 1) << ": " << windSpeed << " m/s\n";
            {
                std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
                GlobalDataStructure::instance().data().lastWS[i] = windSpeed;
                GlobalDataStructure::instance().data().wsHistory[i].push_back(windSpeed);
            }
        } else {
            // std::cerr << "[CommunicationTask] Failed to read wind speed from turbine "
                        // << (i + 1) << "\n"; 
        }

        // Read Wind Direction Measurement From Each Turbine
        float windDirection;
        if (iecWrapper_.rxWindDirection(i + 1, windDirection) == IEC_OK) {
            std::cout << "Received wind direction from turbine " << (i + 1) << ": " << windDirection << " deg\n";
            {
                std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
                GlobalDataStructure::instance().data().lastWD[i] = windDirection;
                GlobalDataStructure::instance().data().wdHistory[i].push_back(windDirection);   
            }
        } else {
            // std::cerr << "[CommunicationTask] Failed to read wind direction from turbine "
                        // << (i + 1) << "\n";  
        }
    }

}

void CommunicationTask::onStop()
{
    state.socket_status.store(COMM_DISCONNECTED);
    socketWrapper.StopOperatorServer();
    socketWrapper.StopAttackInterfaceServer();

    iecWrapper_.stop();
    state.iec_status.store(COMM_DISCONNECTED);

    std::cout << "CommunicationTask: Socket servers stopped.\n";
    std::cout << "CommunicationTask: Stopped\n";
}
