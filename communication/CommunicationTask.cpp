#include "CommunicationTask.hpp"
#include "IECCommunicator.hpp"
#include "socket/SocketWrapper.hpp"
#include "AttackInterface.hpp"
#include "common/config.hpp"
#include "common/util.hpp"

#include <cstring>

CommunicationOrchestrator::CommunicationOrchestrator(const CommConfig& config)
    : config_(config),
      socketWrapper_(config.operatorServer.port,
                     static_cast<int>(config.operatorServer.pollPeriod.count()),
                     config.attackInterface.port,
                     static_cast<int>(config.attackInterface.pollPeriod.count()),
                     config.dataHistorian.port,
                     static_cast<int>(config.dataHistorian.pollPeriod.count())),
      attackInterface_(static_cast<int>(config.mms.turbines.size()), socketWrapper_)
{
    socketStatus_.store(COMM_DISCONNECTED);
    iecStatus_.store(COMM_DISCONNECTED);
}

CommunicationOrchestrator::~CommunicationOrchestrator() = default;

void CommunicationOrchestrator::init()
{
    if (iecWrapper_.init(config_.mms.turbines, config_.goose.networkInterface) != IEC_OK) {
        COMMTASK_ERR("IEC61850 init failed - check CommConfig::mms.turbines");
    }

    socketWrapper_.AttachOpServerCallback([this](const uint8_t* data, size_t length) {
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
        } else if (length >= 5) {
            uint32_t marker = 0;
            std::memcpy(&marker, data, sizeof(marker));
            if (marker != 0x01010101) {
                COMMTASK_ERR("Received operator message with unexpected format or size: " << length << " (data[0] = " << std::to_string(data[0]) << ")");
                return;
            }
            bool simStopped = (*(data + 4) == 0);
            {
                std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
                GlobalDataStructure::instance().data().simStarted = !simStopped;
                if (simStopped) {
                    GlobalDataStructure::instance().data().simConfigured = false;
                    DataHistorian::instance().stopRun();
                }
            }
            COMMTASK_LOG_V1("Received simulation control message from operator server: simStarting = " << !simStopped);
        } else {
            COMMTASK_ERR("Received operator message with unexpected format or size: " << length << " (data[0] = " << (length > 0 ? std::to_string(data[0]) : "N/A") << ")");
        }
    });

    socketWrapper_.AttachDataHistorianCallback([this](const uint8_t* data, size_t length) {
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

    attackInterface_.setCfgCommandCallback([this](const AttackInterface::CfgDataMessage &cmd) {
        COMMTASK_LOG_V1("Received AttackInterface config command: TeamName " << cmd.teamName
                        << ", ScenarioId " << cmd.scenarioId
                        << ", TurbineController " << cmd.turbineController);

        std::lock_guard<std::mutex> lock(attackInterfaceMutex_);
        attackInterface_.resetState();
    });

    attackInterface_.setSimCtrlCommandCallback([this](const AttackInterface::SimCtrlMessage& cmd) {
        COMMTASK_LOG_V1("Received Simulator Control command: simStart " << cmd.simStart);
        int simScenario = 0;
        std::string simTeamName;
        {
            std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
            auto& gds = GlobalDataStructure::instance().data();
            simScenario = gds.simScenario;
            simTeamName = gds.simTeamName;
            if (gds.simConfigured && cmd.simStart) {
                gds.simStarted = true;
            }
        }
        DataHistorian::instance().log("Simulation started with scenario " + std::to_string(simScenario)
                            + " and team " + simTeamName);
    });

    createCommunicators();
}

void CommunicationOrchestrator::createCommunicators()
{
    communicators_.clear();
    communicators_.reserve(config_.mms.turbines.size());

    for (size_t idx = 0; idx < config_.mms.turbines.size(); ++idx) {
        const int turbineId = static_cast<int>(idx) + 1;
        communicators_.push_back(std::make_unique<IECCommunicator>(config_, turbineId, iecWrapper_, attackInterface_, attackInterfaceMutex_));
    }
}

bool CommunicationOrchestrator::start()
{
    socketStatus_.store(COMM_CONNECTING);
    if (socketWrapper_.StartOperatorServer(config_.operatorServer.port) < tcpSOCKET_CONNECTED) {
        COMMTASK_ERR("Failed to start operator server on port " << config_.operatorServer.port);
        return false;
    }
    if (socketWrapper_.StartAttackInterfaceServer(config_.attackInterface.port) < tcpSOCKET_CONNECTED) {
        COMMTASK_ERR("Failed to start attack interface server on port " << config_.attackInterface.port);
        return false;
    }
    if (socketWrapper_.StartDataHistorianServer(config_.dataHistorian.port) < tcpSOCKET_CONNECTED) {
        COMMTASK_ERR("Failed to start data historian server on port " << config_.dataHistorian.port);
        return false;
    }
    socketStatus_.store(COMM_CONNECTED);

    iecStatus_.store(COMM_CONNECTING);
    iecWrapper_.start();
    iecStatus_.store(COMM_CONNECTED);

    for (auto& communicator : communicators_) {
        communicator->start();
    }

    return true;
}

void CommunicationOrchestrator::stop()
{
    for (auto& communicator : communicators_) {
        communicator->stop();
    }

    socketWrapper_.StopOperatorServer();
    socketWrapper_.StopAttackInterfaceServer();
    socketWrapper_.StopDataHistorianServer();
    socketStatus_.store(COMM_DISCONNECTED);

    iecWrapper_.stop();
    iecStatus_.store(COMM_DISCONNECTED);
}

std::vector<CommunicationOrchestrator::CommunicatorState> CommunicationOrchestrator::communicatorStates() const
{
    std::vector<CommunicatorState> states;
    states.reserve(communicators_.size());
    for (const auto& communicator : communicators_) {
        states.push_back({communicator->turbineId(), communicator->status(), communicator->lastActivityTime()});
    }
    return states;
}

CommStatus CommunicationOrchestrator::socketStatus() const
{
    return socketStatus_.load();
}

CommStatus CommunicationOrchestrator::iecStatus() const
{
    return iecStatus_.load();
}
