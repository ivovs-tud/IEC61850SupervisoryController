#include "CommunicationTask.hpp"
#include "socket/SocketWrapper.hpp"
#include "AttackInterface.hpp"

CommunicationTask::CommunicationTask(const CommConfig& config)
    : PeriodicTask(config.orchestrationPeriod)
    , config_(config), attackInterface(config.mms.turbines.size(), socketWrapper)
{
    // TODO: construct libiec_wrapper and SocketWrapper instances

    state.iec_status.store(COMM_DISCONNECTED);
    state.socket_status.store(COMM_DISCONNECTED);
    state.lastActivityTime = std::chrono::system_clock::now();
    // attackInterface = AttackInterface::AttackInterface(config.mms.turbines.size(), socketWrapper);
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




// =============================================================================
// Static descriptor tables
// To add a new measurement: append a row to RX_DESCRIPTORS.
// To add a new setpoint:    append a row to TX_DESCRIPTORS.
// =============================================================================

const CommunicationTask::RxDescriptor CommunicationTask::RX_DESCRIPTORS[] = {
    //  name              unit   IEC read fn                         AI type                   GDS last-value field     GDS history field
    { "wind speed",      "m/s",  &libiec_wrapper::rxWindSpeed,      AttackInterface::TX_WS,    &GlobalData::lastWS,     &GlobalData::wsHistory  },
    { "wind direction",  "deg",  &libiec_wrapper::rxWindDirection,  AttackInterface::TX_WD,    &GlobalData::lastWD,     &GlobalData::wdHistory  },
    { "rotor speed",     "RPM",  &libiec_wrapper::rxRotorSpeed,     AttackInterface::TX_RPM,   &GlobalData::lastRPM,    &GlobalData::rpmHistory },
};

const CommunicationTask::TxDescriptor CommunicationTask::TX_DESCRIPTORS[] = {
    //  name            unit    GDS reader                                                                                  AI type                         IEC write fn
    { "power setpoint", "W",    [](const GlobalData& d, int i) { return d.TurbinePowerSetpoints[i]; },                      AttackInterface::TX_SPT_PWR,    &libiec_wrapper::txPowerSetpoint },
    { "yaw setpoint",   "deg",  [](const GlobalData& d, int i) { return static_cast<float>(d.TurbineYawSetpoints[i]); },    AttackInterface::TX_SPT_YAW,    &libiec_wrapper::txYawSetpoint   },
};

// =============================================================================

void CommunicationTask::execute()
{
    for (int i = 0; i < static_cast<int>(config_.mms.turbines.size()); ++i) {
        if (i > 0) continue;
        const int turbineId = i + 1;  // 1-based for IEC 61850

        for (const auto& desc : TX_DESCRIPTORS)
            doTxSetpoint(turbineId, i, desc);

        doRxSecret(turbineId);

        for (const auto& desc : RX_DESCRIPTORS)
            doRxMeasurement(turbineId, i, desc);
    }
}

// -----------------------------------------------------------------------------
// Unified per-turbine operation helpers
// -----------------------------------------------------------------------------

void CommunicationTask::doTxSetpoint(int turbineId, int idx, const TxDescriptor& desc)
{
    float value;
    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        value = desc.gdsRead(GlobalDataStructure::instance().data(), idx);
    }

    attackInterface.txData(turbineId, desc.txDataType, value);

    // Before storing, potentially allow attackInterface to overwrite the measurement
    if(attackInterface.overwrite(turbineId, desc.txDataType, value) < 0) {
        std::cerr << "[CommunicationTask] Failed to get overwrite decision for " << desc.name
                  << " from turbine " << turbineId << "\n";
    }

    if ((iecWrapper_.*desc.iecWrite)(turbineId, value) != IEC_OK) {
        std::cerr << "[CommunicationTask] Failed to write " << desc.name
                  << " to turbine " << turbineId << "\n";
    } else {
        std::cout << "Sent " << desc.name << " to turbine " << turbineId
                  << ": " << value << " " << desc.unit << "\n";
    }
}

void CommunicationTask::doRxSecret(int turbineId)
{
    std::string secret;
    if (iecWrapper_.rxSecret(turbineId, secret) == IEC_OK) {
        std::cout << "Received secret from turbine " << turbineId << ": " << secret << "\n";
    } else {
        std::cerr << "[CommunicationTask] Failed to read secret from turbine " << turbineId << "\n";
    }
}

void CommunicationTask::doRxMeasurement(int turbineId, int idx, const RxDescriptor& desc)
{
    float value;
    // First request the data from a turbine IEC 61850 server
    if ((iecWrapper_.*desc.iecRead)(turbineId, value) != IEC_OK) {
        std::cerr << "[CommunicationTask] Failed to read " << desc.name
                  << " from turbine " << turbineId << "\n";
        return;
    }

    std::cout << "Received " << desc.name << " from turbine " << turbineId
              << ": " << value << " " << desc.unit << "\n";

    // Then potentially transmit this data to an eavesdropper over the attack interface
    attackInterface.txData(turbineId, desc.txDataType, value);

    // Before storing, potentially allow attackInterface to overwrite the measurement
    if(attackInterface.overwrite(turbineId, desc.txDataType, value) < 0) {
        std::cerr << "[CommunicationTask] Failed to get overwrite decision for " << desc.name
                  << " from turbine " << turbineId << "\n";
    }
    std::cout << "Received " << desc.name << " from turbine " << turbineId
              << ": " << value << " " << desc.unit << "\n";

    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        auto& gds = GlobalDataStructure::instance().data();
        (gds.*desc.lastField)[idx]    = value;
        (gds.*desc.historyField)[idx].push_back(value);
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
