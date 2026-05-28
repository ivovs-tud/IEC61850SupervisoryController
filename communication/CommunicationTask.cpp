#include "CommunicationTask.hpp"
#include "socket/SocketWrapper.hpp"
#include "AttackInterface.hpp"
#include "common/config.hpp"
#include "common/util.hpp"

#include <cstring>
#include <future>
#include <map>
#include <string>
#include <vector>

CommunicationTask::CommunicationTask(const CommConfig& config)
    : PeriodicTask(config.orchestrationPeriod), config_(config), 
        socketWrapper(
            config.operatorServer.port, static_cast<int>(config.operatorServer.pollPeriod.count()),
            config.attackInterface.port, static_cast<int>(config.attackInterface.pollPeriod.count()),
            config.dataHistorian.port, static_cast<int>(config.dataHistorian.pollPeriod.count())), 
        attackInterface(config.mms.turbines.size(), socketWrapper) {
    state.iec_status.store(COMM_DISCONNECTED);
    state.socket_status.store(COMM_DISCONNECTED);
    state.lastActivityTime = std::chrono::system_clock::now();
}

void CommunicationTask::init()
{
    // Initialise IEC 61850 connections for all turbines listed in CommConfig.
    if (iecWrapper_.init(config_.mms.turbines, config_.goose.networkInterface) != IEC_OK)
    {
        COMMTASK_ERR("IEC61850 init failed - check CommConfig::mms.turbines");
    }

    socketWrapper.AttachOpServerCallback([this](const uint8_t* data, size_t length) {
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
// Descriptor Execution Time Control Methods
// Each (turbineId, descriptor*) pair gets its own execution time entry.
// =============================================================================

uint64_t CommunicationTask::getRxNextExecutionTimeMs(int turbineId, const RxDescriptor* desc) const
{
    std::lock_guard<std::mutex> lock(rxDescriptorMutex_);
    auto key = std::make_pair(turbineId, desc);
    auto it = rxNextExecutionTimes_.find(key);
    if (it == rxNextExecutionTimes_.end()) {
        // First time: initialize to now (allow immediate execution)
        return 0;
    }
    return it->second;
}

uint64_t CommunicationTask::getTxNextExecutionTimeMs(int turbineId, const TxDescriptor* desc) const
{
    std::lock_guard<std::mutex> lock(txDescriptorMutex_);
    auto key = std::make_pair(turbineId, desc);
    auto it = txNextExecutionTimes_.find(key);
    if (it == txNextExecutionTimes_.end()) {
        // First time: initialize to now (allow immediate execution)
        return 0;
    }
    return it->second;
}

void CommunicationTask::setRxNextExecutionTimeMs(int turbineId, const RxDescriptor* desc, uint64_t timeMs)
{
    std::lock_guard<std::mutex> lock(rxDescriptorMutex_);
    auto key = std::make_pair(turbineId, desc);
    rxNextExecutionTimes_[key] = timeMs;
    COMMTASK_LOG_V2("RX descriptor '" << desc->name << "' (turbine " << turbineId << ") next execution time set to " << timeMs << " ms");
}

void CommunicationTask::setTxNextExecutionTimeMs(int turbineId, const TxDescriptor* desc, uint64_t timeMs)
{
    std::lock_guard<std::mutex> lock(txDescriptorMutex_);
    auto key = std::make_pair(turbineId, desc);
    txNextExecutionTimes_[key] = timeMs;
    COMMTASK_LOG_V2("TX descriptor '" << desc->name << "' (turbine " << turbineId << ") next execution time set to " << timeMs << " ms");
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
    //  name        unit    IEC read fn                         AI type                   GDS last-value field     GDS history field          Interval (ms)
    { "V",          "m/s",  &libiec_wrapper::rxWindSpeed,      AttackInterface::TX_WS,    &GlobalData::lastWS,     &GlobalData::wsHistory,    &GlobalData::lastWS_t, 500 },
    { "D",          "deg",  &libiec_wrapper::rxWindDirection,  AttackInterface::TX_WD,    &GlobalData::lastWD,     &GlobalData::wdHistory,    &GlobalData::lastWD_t, 500 },
    { "YawMeas",    "deg",  &libiec_wrapper::rxYawOffset,      AttackInterface::TX_YAW,   &GlobalData::lastYawOffset, &GlobalData::yawOffsetHistory, &GlobalData::lastYawOffset_t, 500 },
    { "RSpd",       "RPM",  &libiec_wrapper::rxRotorSpeed,     AttackInterface::TX_RPM,   &GlobalData::lastRPM,    &GlobalData::rpmHistory,   &GlobalData::lastRPM_t, 500  },
    { "W",          "W",    &libiec_wrapper::rxPowerGen,       AttackInterface::TX_PW,    &GlobalData::lastPower,    &GlobalData::powerHistory, &GlobalData::lastPower_t, 500  },
};

const CommunicationTask::TxDescriptor CommunicationTask::TX_DESCRIPTORS[] = {
    //  name        unit    GDS reader                                                                AI type                         IEC write fn                            Interval (ms)
    { "WSpt",       IEC_FLOAT32, [](GlobalData& d, int i)->void* { return &d.TurbinePowerSetpoints[i]; },  AttackInterface::TX_SPT_PWR,    &libiec_wrapper::txPowerSetpoint,       5000},
    { "YawSpt",     IEC_FLOAT32, [](GlobalData& d, int i)->void* { return &d.TurbineYawSetpoints[i]; },    AttackInterface::TX_SPT_YAW,    &libiec_wrapper::txYawSetpoint,         1000 },
    { "OP_CMD",     IEC_UINT32,  [](GlobalData& d, int i)->void* { return &d.enableTurbine[i]; },         AttackInterface::TX_NONE,       &libiec_wrapper::txOpCommand,           1000 },
    { "TUR_CTL",    IEC_UINT32,  [](GlobalData& d, int i)->void* { return &d.TurbineController[i]; },      AttackInterface::TX_NONE,       &libiec_wrapper::txTurbineController,   1000 },
};

// =============================================================================

void CommunicationTask::execute()
{
    const int turbineCount = static_cast<int>(config_.mms.turbines.size());
    if (turbineCount <= 1) {
        for (int i = 0; i < turbineCount; ++i) {
            processTurbine(i + 1, i);
        }
        return;
    }

    std::vector<std::future<void>> futures;
    futures.reserve(turbineCount);

    for (int i = 0; i < turbineCount; ++i) {
        futures.emplace_back(std::async(std::launch::async, &CommunicationTask::processTurbine, this, i + 1, i));
    }

    for (auto& future : futures) {
        future.wait();
    }
}

void CommunicationTask::processTurbine(int turbineId, int idx)
{
    const uint64_t currentTimeMs = getCurrentTimeMs();
    const int txDescCount = sizeof(TX_DESCRIPTORS) / sizeof(TX_DESCRIPTORS[0]);
    const int rxDescCount = sizeof(RX_DESCRIPTORS) / sizeof(RX_DESCRIPTORS[0]);

    for (int i = 0; i < txDescCount; ++i) {
        const TxDescriptor* desc = &TX_DESCRIPTORS[i];
        const uint64_t nextExecTime = getTxNextExecutionTimeMs(turbineId, desc);
        if (currentTimeMs >= nextExecTime) {
            doTxSetpoint(turbineId, idx, *desc);
            setTxNextExecutionTimeMs(turbineId, desc, currentTimeMs + desc->intervalMs);
        }
    }

    for (int i = 0; i < rxDescCount; ++i) {
        const RxDescriptor* desc = &RX_DESCRIPTORS[i];
        const uint64_t nextExecTime = getRxNextExecutionTimeMs(turbineId, desc);
        if (currentTimeMs >= nextExecTime) {
            doRxMeasurement(turbineId, idx, *desc);
            setRxNextExecutionTimeMs(turbineId, desc, currentTimeMs + desc->intervalMs);
        } 
        // else if (idx == 0){
        //     COMMTASK_ST("Skipping RX descriptor '" << desc->name << "' for turbine " << turbineId 
        //                     << " (current time " << currentTimeMs << " ms, next exec time " << nextExecTime << " ms)");
        // }
    }
}

// -----------------------------------------------------------------------------
// Unified per-turbine operation helpers
// -----------------------------------------------------------------------------

void CommunicationTask::doTxSetpoint(int turbineId, int idx, const TxDescriptor& desc)
{
    // Acquire pointer to the value inside the global data structure
    void* value = nullptr;
    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        value = desc.gdsPtr(GlobalDataStructure::instance().data(), idx);
    }

    std::string logMsg = "[SC→WT" + std::to_string(turbineId) + "]" + std::to_string(getCurrentTimeMs()) + ";" + descToString(value, desc);
    DataHistorian::instance().log(logMsg);

    {
        std::lock_guard<std::mutex> lock(attackInterfaceMutex_);
        attackInterface.txData(turbineId, desc.txDataType, value);

        if (desc.type == IEC_FLOAT32) {
            float& f = *static_cast<float*>(value);
            if (attackInterface.overwrite(turbineId, desc.txDataType, f) < 0) {
                COMMTASK_ERR("Failed to get overwrite decision for " << desc.name
                             << " from turbine " << turbineId);
            }
        }
    }

    logMsg = "[SC→WT" + std::to_string(turbineId) + "(A)]" + std::to_string(getCurrentTimeMs()) + ";" + descToString(value, desc);
    DataHistorian::instance().log(logMsg);

    if ((iecWrapper_.*desc.iecWrite)(turbineId, value) != IEC_OK) {
        COMMTASK_ERR("Failed to write " << desc.name << " to turbine " << turbineId);
    } else {
        COMMTASK_LOG_V1("Sent " << desc.name << " to turbine " << turbineId
                        << ": " << descToString(value, desc));
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

    {
        std::lock_guard<std::mutex> lock(attackInterfaceMutex_);
        attackInterface.txData(turbineId, desc.txDataType, &value);

        if (attackInterface.overwrite(turbineId, desc.txDataType, value) < 0) {
            COMMTASK_ERR("Failed to get overwrite decision for " << desc.name << " from turbine " << turbineId);
        }
    }

    // If we requested power, we store the true value too, since it is used later to compute the total power generated as measured here.
    if (strcmp(desc.name, "W") == 0) {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        auto& gds = GlobalDataStructure::instance().data();
        gds._W[turbineId - 1] = value;
    }

    COMMTASK_LOG_V1("Received (post-overwrite) " << desc.name << " for turbine " << turbineId << ": " << value << " " << desc.unit);
    logMsg = "[WT" + std::to_string(turbineId) + "→SC(A)]" + std::to_string(getCurrentTimeMs()) + ";" + desc.name + "=" + std::to_string(value);
    DataHistorian::instance().log(logMsg);

    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        auto& gds = GlobalDataStructure::instance().data();
        (gds.*desc.lastField)[idx]    = value;
        (gds.*desc.historyField)[idx].push_back(value);
        (gds.*desc.lastTimestamp)[idx] = getCurrentTimeMs();
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
