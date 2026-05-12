#include "CommunicationTask.hpp"
#include "socket/SocketWrapper.hpp"
#include "AttackInterface.hpp"
#include "common/config.hpp"
#include "common/TimeUtils.hpp"

#include <cstring>
#include <map>
#include <string>

CommunicationTask::CommunicationTask(const CommConfig& config)
    : PeriodicTask(config.orchestrationPeriod)
    , config_(config)
    , socketWrapper(config.operatorServer.port,
                    static_cast<int>(config.operatorServer.pollPeriod.count()),
                    config.attackInterface.port,
                    static_cast<int>(config.attackInterface.pollPeriod.count()),
                    config.dataHistorian.port,
                    static_cast<int>(config.dataHistorian.pollPeriod.count()))
    , attackInterface(config.mms.turbines.size(), socketWrapper)
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
        COMMTASK_ERR("IEC61850 init failed - check CommConfig::mms.turbines");
    }

    socketWrapper.AttachServerCallback([this](const uint8_t* data, size_t length) {
        // Reinterpret a uint32_t's bit pattern as an IEEE 754 float.
        auto asFloat = [](const uint8_t *u) {
            float f;
            std::memcpy(&f, u, sizeof(f));
            return f;
        };
        if (length == 4) {
            const float value = asFloat(data);
            std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
            GlobalDataStructure::instance().data().RequestedReferencePower = value;
            COMMTASK_LOG_V1("Updated RequestedReferencePower to " << value);
        } else if (length >= 5 && (*reinterpret_cast<const uint32_t*>(data) == 0x01010101)) {
            // This means the message is a synchronization message from the operator server
            // The next value indicates if the simulation is stopping (0.0) or starting (!= 0.0)
            bool simStopped = (*(data + 4) == 0);
            {
                std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
                if (simStopped) {
                    // If simulation is stopping, also reset simConfigured to require reconfiguration for the next run
                    GlobalDataStructure::instance().data().simStarted = false;
                    GlobalDataStructure::instance().data().simConfigured = false;
                    DataHistorian::instance().stopRun();
                }
            }
            COMMTASK_LOG_V1("Received simulation control message from operator server: simStarting = " << !simStopped);
        } else {
            COMMTASK_ERR("Received operator message with unexpected format or size: " << length << " (data[0] = " << (length > 0 ? std::to_string(data[0]) : "N/A") << ")");
        }
    });

    socketWrapper.AttachDataHistorianCallback([this](const uint8_t* data, size_t length) {
        if (data == nullptr || length == 0) {
            COMMTASK_ERR("Received empty DataHistorian message");
            return;
        }

        COMMTASK_LOG_V2("Received DataHistorian message of size " << length << " bytes");
        DH_TCP_DATA out;
        if (length < sizeof(DH_TCP_DATA)) {
               COMMTASK_ERR("Received Invalid DataHistorian Message Length. Expected " << sizeof(DH_TCP_DATA) << " bytes, got " << length << " bytes");
            return;
        }

        memcpy(&out, data, sizeof(DH_TCP_DATA));

		char logMsg[512];
        snprintf(logMsg, sizeof(logMsg), "[WT%u]%lu;YawAng=%.1f;YawSpt=%.1f;W=%.1f;WSpt=%.1f;V=%.1f;D=%.1f;RotSpd=%.1f;Pth=%.1f;PthSpt=%.1f",
			out.nID, out.nUnixTime, out.YwAng, out.YwAngSpt, out.W, out.WSpt, out.HorWdSpd, out.HorWdDir, out.RotSpd, out.PitchAngle, out.PitchAngleSpt);

        DataHistorian::instance().log(std::string(logMsg));
    });

    attackInterface.setCfgCommandCallback([this](const AttackInterface::CfgDataMessage &cmd) {
        // Handle configuration commands from the test harness (e.g., enable/disable attack interface control for specific data types)
        COMMTASK_LOG_V1("Received AttackInterface config command: TeamName " << cmd.teamName 
                        << ", ScenarioId " << cmd.scenarioId 
                        << ", TurbineController " << cmd.turbineController);

        // GlobalDataStructure::instance().resetForNewRun(std::string(cmd.teamName), cmd.scenarioId, cmd.turbineController);

        attackInterface.resetState();

        // TODO: also send scenario ID to simulator PC and send turbineControllerId to Speedgoat
    });

    attackInterface.setSimCtrlCommandCallback([this](const AttackInterface::SimCtrlMessage& cmd) {
        // Handle simulator control commands from the test harness (e.g., start/stop simulation, switch scenarios)
        COMMTASK_LOG_V1("Received Simulator Control command: simStart " << cmd.simStart);
        DataHistorian::instance().log("Simulation started with scenario " + std::to_string(GlobalDataStructure::instance().data().simScenario) 
                            + " and team " + GlobalDataStructure::instance().data().simTeamName);
        {
            std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
            auto& gds = GlobalDataStructure::instance().data();
            if (gds.simConfigured && cmd.simStart) {
                gds.simStarted = true;
            }
        }

        // TODO: Also send start sim command to simulator PC
    });

    // TODO: set up GOOSE subscriber via libiec_wrapper and register onGooseMessage() as callback
}

// =============================================================================
// Descriptor Timestamp Control Methods
// =============================================================================

std::string CommunicationTask::getDescriptorKey(int turbineId, const char* descriptorName) const
{
    return std::to_string(turbineId) + ":" + std::string(descriptorName);
}

uint64_t CommunicationTask::getRxNextExecutionTimeMs(int turbineId, const RxDescriptor& desc) const
{
    std::lock_guard<std::mutex> lock(rxDescriptorMutex_);
    std::string key = getDescriptorKey(turbineId, desc.name);
    auto it = rxNextExecutionTimes_.find(key);
    if (it == rxNextExecutionTimes_.end()) {
        // First time: initialize to now (allow immediate execution)
        return getCurrentTimeMs();
    }
    return it->second;
}

uint64_t CommunicationTask::getTxNextExecutionTimeMs(int turbineId, const TxDescriptor& desc) const
{
    std::lock_guard<std::mutex> lock(txDescriptorMutex_);
    std::string key = getDescriptorKey(turbineId, desc.name);
    auto it = txNextExecutionTimes_.find(key);
    if (it == txNextExecutionTimes_.end()) {
        // First time: initialize to now (allow immediate execution)
        return getCurrentTimeMs();
    }
    return it->second;
}

void CommunicationTask::setRxNextExecutionTimeMs(int turbineId, const RxDescriptor& desc, uint64_t timeMs)
{
    std::lock_guard<std::mutex> lock(rxDescriptorMutex_);
    std::string key = getDescriptorKey(turbineId, desc.name);
    rxNextExecutionTimes_[key] = timeMs;
    COMMTASK_LOG_V2("RX descriptor '" << desc.name << "' (turbine " << turbineId << ") next execution time set to " << timeMs << " ms");
}

void CommunicationTask::setTxNextExecutionTimeMs(int turbineId, const TxDescriptor& desc, uint64_t timeMs)
{
    std::lock_guard<std::mutex> lock(txDescriptorMutex_);
    std::string key = getDescriptorKey(turbineId, desc.name);
    txNextExecutionTimes_[key] = timeMs;
    COMMTASK_LOG_V2("TX descriptor '" << desc.name << "' (turbine " << turbineId << ") next execution time set to " << timeMs << " ms");
}

void CommunicationTask::onStart()
{
    state.socket_status.store(COMM_CONNECTING);
    if (socketWrapper.StartOperatorServer(config_.operatorServer.port) < tcpSOCKET_CONNECTED) {
        COMMTASK_ERR("Failed to start operator server on port " << config_.operatorServer.port);
    }
    if (socketWrapper.StartAttackInterfaceServer(config_.attackInterface.port) < tcpSOCKET_CONNECTED) {
        COMMTASK_ERR("Failed to start attack interface server on port " << config_.attackInterface.port);
    }
    if (socketWrapper.StartDataHistorianServer(config_.dataHistorian.port) < tcpSOCKET_CONNECTED) {
        COMMTASK_ERR("Failed to start data historian server on port " << config_.dataHistorian.port);
    }

    state.socket_status.store(COMM_CONNECTED);

    state.iec_status.store(COMM_CONNECTING);
    iecWrapper_.start();
    state.iec_status.store(COMM_CONNECTED);

    // iecWrapper_.printTurbineDataModel(1, 1000);  // print first 10 DA references of turbine 1 for sanity check

#if SC_LOG_LEVEL_COMMTASK >= 3
    auto support = iecWrapper_.checkTurbineSupport(1, IEC_STRINGS::REQ_REFS);
    for (auto& [ref, ok] : support)
        COMMTASK_LOG_V2(ref << ": " << (ok ? "OK" : "NOT SUPPORTED"));

    support = iecWrapper_.checkTurbineSupport(1, IEC_STRINGS::REQ_CMDS);
    for (auto& [ref, ok] : support)
        COMMTASK_LOG_V2(ref << ": " << (ok ? "OK" : "NOT SUPPORTED"));
#endif

    
}




// =============================================================================
// Static descriptor tables
// To add a new measurement: append a row to RX_DESCRIPTORS.
// To add a new setpoint:    append a row to TX_DESCRIPTORS.
// =============================================================================

const CommunicationTask::RxDescriptor CommunicationTask::RX_DESCRIPTORS[] = {
    //  name        unit    IEC read fn                         AI type                   GDS last-value field     GDS history field          interval (ms)
    { "V",          "m/s",  &libiec_wrapper::rxWindSpeed,      AttackInterface::TX_WS,    &GlobalData::lastWS,     &GlobalData::wsHistory,    1000 },
    { "D",          "deg",  &libiec_wrapper::rxWindDirection,  AttackInterface::TX_WD,    &GlobalData::lastWD,     &GlobalData::wdHistory,    1000 },
    { "YawMeas",    "deg",  &libiec_wrapper::rxYawOffset,      AttackInterface::TX_YAW,   &GlobalData::lastYawOffset, &GlobalData::yawOffsetHistory, 2000 },
    { "RSpd",       "RPM",  &libiec_wrapper::rxRotorSpeed,     AttackInterface::TX_RPM,   &GlobalData::lastRPM,    &GlobalData::rpmHistory,   2000  },
    { "W",          "W",    &libiec_wrapper::rxPowerGen,       AttackInterface::TX_PW,    &GlobalData::Power_i,    &GlobalData::powerHistory, 500  },
};

const CommunicationTask::TxDescriptor CommunicationTask::TX_DESCRIPTORS[] = {
    //  name        unit    GDS reader                                                                                  AI type                         IEC write fn                        interval (ms)
    { "WSpt",       "W",    [](const GlobalData& d, int i) { return d.TurbinePowerSetpoints[i]; },                      AttackInterface::TX_SPT_PWR,    &libiec_wrapper::txPowerSetpoint,   5000 },
    { "YawSpt",     "deg",  [](const GlobalData& d, int i) { return static_cast<float>(d.TurbineYawSetpoints[i]); },    AttackInterface::TX_SPT_YAW,    &libiec_wrapper::txYawSetpoint,     1000 },
    { "OP_CMD",     "",     [](const GlobalData& d, int i) { return 1; },                                               AttackInterface::TX_OP_CMD,     &libiec_wrapper::txOpCommand,       UINT32_MAX },
};

// =============================================================================

void CommunicationTask::execute()
{

    for (int i = 0; i < static_cast<int>(config_.mms.turbines.size()); ++i) {
        // if (i > 0) continue;
        const int turbineId = i + 1;  // 1-based for IEC 61850

        // TX operations: check timestamp before executing.
        // Capture the current time immediately before each comparison so that
        // first-use descriptors (which default to "now") are handled
        // deterministically instead of occasionally missing their first cycle.
        for (const auto& desc : TX_DESCRIPTORS) {
            const uint64_t nextExecTime = getTxNextExecutionTimeMs(turbineId, desc);
            const uint64_t currentTimeMs = getCurrentTimeMs();
            if (currentTimeMs >= nextExecTime) {
                doTxSetpoint(turbineId, i, desc);
                setTxNextExecutionTimeMs(turbineId, desc, currentTimeMs + desc.intervalMs);
            }
        }

        // RX operations: check timestamp before executing
        // doRxSecret(turbineId);

        for (const auto& desc : RX_DESCRIPTORS) {
            const uint64_t nextExecTime = getRxNextExecutionTimeMs(turbineId, desc);
            const uint64_t currentTimeMs = getCurrentTimeMs();
            if (currentTimeMs >= nextExecTime) {
                doRxMeasurement(turbineId, i, desc);
                setRxNextExecutionTimeMs(turbineId, desc, currentTimeMs + desc.intervalMs);
            }
        }
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

    std::string logMsg = "[SC→WT" + std::to_string(turbineId) + "]" + std::to_string(getCurrentTimeMs()) + ";" + desc.name + "=" + std::to_string(value);
    DataHistorian::instance().log(logMsg);

    attackInterface.txData(turbineId, desc.txDataType, value);

    // Before storing, potentially allow attackInterface to overwrite the measurement
    if(attackInterface.overwrite(turbineId, desc.txDataType, value) < 0) {
        COMMTASK_ERR("Failed to get overwrite decision for " << desc.name
                     << " from turbine " << turbineId);
    }

    logMsg = "[SC→WT" + std::to_string(turbineId) + "(A)]" + std::to_string(getCurrentTimeMs()) + ";" + desc.name + "=" + std::to_string(value);
    DataHistorian::instance().log(logMsg);

    if ((iecWrapper_.*desc.iecWrite)(turbineId, value) != IEC_OK) {
        COMMTASK_ERR("Failed to write " << desc.name << " to turbine " << turbineId);
    } else {
        COMMTASK_LOG_V1("Sent " << desc.name << " to turbine " << turbineId
                        << ": " << value << " " << desc.unit);
    }
}

void CommunicationTask::doRxSecret(int turbineId)
{
    std::string secret;
    if (iecWrapper_.rxSecret(turbineId, secret) == IEC_OK) {
        COMMTASK_LOG_V2("Received secret from turbine " << turbineId << ": " << secret);
    } else {
        COMMTASK_ERR("Failed to read secret from turbine " << turbineId);
    }
}

void CommunicationTask::doRxMeasurement(int turbineId, int idx, const RxDescriptor& desc)
{
    float value;
    // First request the data from a turbine IEC 61850 server
    if ((iecWrapper_.*desc.iecRead)(turbineId, value) != IEC_OK) {
        COMMTASK_ERR("Failed to read " << desc.name << " from turbine " << turbineId);
        return;
    }

    COMMTASK_LOG_V2("Received (pre-overwrite)" << desc.name << " from turbine " << turbineId << ": " << value << " " << desc.unit);
    std::string logMsg = "[WT" + std::to_string(turbineId) + "→SC]" + std::to_string(getCurrentTimeMs()) + ";" + desc.name + "=" + std::to_string(value);
    DataHistorian::instance().log(logMsg);

    // Then potentially transmit this data to an eavesdropper over the attack interface
    attackInterface.txData(turbineId, desc.txDataType, value);

    // Before storing, potentially allow attackInterface to overwrite the measurement
    if(attackInterface.overwrite(turbineId, desc.txDataType, value) < 0) {
        COMMTASK_ERR("Failed to get overwrite decision for " << desc.name << " from turbine " << turbineId);
    }
    COMMTASK_LOG_V1("Received (post-overwrite) " << desc.name << " for turbine " << turbineId << ": " << value << " " << desc.unit);
    logMsg = "[WT" + std::to_string(turbineId) + "→SC(A)]" + std::to_string(getCurrentTimeMs()) + ";" + desc.name + "=" + std::to_string(value);
    DataHistorian::instance().log(logMsg);

    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        auto& gds = GlobalDataStructure::instance().data();
        (gds.*desc.lastField)[idx]    = value;
        (gds.*desc.historyField)[idx].push_back(value);
    }
}

void CommunicationTask::onStop()
{
    
    socketWrapper.StopOperatorServer();
    socketWrapper.StopAttackInterfaceServer();
    socketWrapper.StopDataHistorianServer();
    state.socket_status.store(COMM_DISCONNECTED);


    iecWrapper_.stop();
    state.iec_status.store(COMM_DISCONNECTED);

    //COMMTASK_ST("Socket servers stopped.");
    COMMTASK_ST("Stopped");
}
