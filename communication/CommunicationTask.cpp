#include "CommunicationTask.hpp"
#include "IECCommunicator.hpp"
#include "socket/SocketWrapper.hpp"
#include "AttackInterface.hpp"
#include "common/config.hpp"
#include "common/util.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

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
    socketWrapper_.setFailureHandler([this](const std::string& message) {
        handleRuntimeFailure(message);
    });
}

CommunicationOrchestrator::~CommunicationOrchestrator()
{
    stop();
}

CommunicationOrchestrator::StartupResult CommunicationOrchestrator::init()
{
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (initialized_) {
        return {true, StartupStage::None, {}};
    }
    if (iecWrapper_.init(config_.mms.turbines, config_.goose.networkInterface) != IEC_OK) {
        COMMTASK_ERR("IEC61850 init failed - check CommConfig::mms.turbines");
        return {false, StartupStage::Initialization, "failed to initialize IEC 61850 wrapper"};
    }

    socketWrapper_.AttachOpServerCallback([this](const uint8_t* data, size_t length) {
        auto asFloat = [](const uint8_t *u) {
            float f;
            std::memcpy(&f, u, sizeof(f));
            return f;
        };

        if (length == 4) {
            const float value = asFloat(data);
            {
                auto& control = SharedData::instance().control;
                std::lock_guard<std::mutex> lock(control.mutex);
                control.requestedPower = value;
            }
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
                auto& interface = SharedData::instance().interface;
                std::lock_guard<std::mutex> lock(interface.mutex);
                interface.simStarted = !simStopped;
                if (simStopped) {
                    interface.simConfigured = false;
                }
            }
            if (simStopped) {
                DataHistorian::instance().stopRun();
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
        // Logging may compile out.
        (void)cmd;
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
            auto& interface = SharedData::instance().interface;
            std::lock_guard<std::mutex> lock(interface.mutex);
            simScenario = interface.simScenario;
            simTeamName = interface.simTeamName;
            if (interface.simConfigured && cmd.simStart) {
                interface.simStarted = true;
            }
        }
        DataHistorian::instance().log("Simulation started with scenario " + std::to_string(simScenario)
                            + " and team " + simTeamName);
    });

    createCommunicators();
    initialized_ = true;
    return {true, StartupStage::None, {}};
}

void CommunicationOrchestrator::createCommunicators()
{
    communicators_.clear();
    communicators_.reserve(config_.mms.turbines.size());

    for (size_t idx = 0; idx < config_.mms.turbines.size(); ++idx) {
        const int turbineId = static_cast<int>(idx) + 1;
        auto communicator = std::make_unique<IECCommunicator>(
            config_, turbineId, iecWrapper_, attackInterface_, attackInterfaceMutex_);
        communicator->setFailureHandler([this](const std::string& message) {
            handleRuntimeFailure(message);
        });
        communicators_.push_back(std::move(communicator));
    }
}

CommunicationOrchestrator::StartupResult CommunicationOrchestrator::start()
{
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (!initialized_) {
        return {false, StartupStage::Initialization, "communication is not initialized"};
    }
    if (started_) {
        return {false, StartupStage::AlreadyRunning, "communication is already running"};
    }

    socketStatus_.store(COMM_CONNECTING);
    if (socketWrapper_.StartOperatorServer(config_.operatorServer.port) < tcpSOCKET_CONNECTED) {
        COMMTASK_ERR("Failed to start operator server on port " << config_.operatorServer.port);
        rollbackStart(0);
        return {false, StartupStage::OperatorServer, "failed to start operator server"};
    }
    operatorStarted_ = true;
    if (socketWrapper_.StartAttackInterfaceServer(config_.attackInterface.port) < tcpSOCKET_CONNECTED) {
        COMMTASK_ERR("Failed to start attack interface server on port " << config_.attackInterface.port);
        rollbackStart(0);
        return {false, StartupStage::AttackInterface, "failed to start attack interface server"};
    }
    attackStarted_ = true;
    if (socketWrapper_.StartDataHistorianServer(config_.dataHistorian.port) < tcpSOCKET_CONNECTED) {
        COMMTASK_ERR("Failed to start data historian server on port " << config_.dataHistorian.port);
        rollbackStart(0);
        return {false, StartupStage::DataHistorian, "failed to start data historian server"};
    }
    dataHistorianStarted_ = true;
    socketStatus_.store(COMM_CONNECTED);

    iecStatus_.store(COMM_CONNECTING);
    if (iecWrapper_.start() != IEC_OK) {
        rollbackStart(0);
        return {false, StartupStage::Iec, "failed to start IEC 61850 wrapper"};
    }
    iecStarted_ = true;
    iecStatus_.store(
        iecWrapper_.connectionStatus() == IEC_LINK_CONNECTED
            ? COMM_CONNECTED
            : COMM_CONNECTING);

    for (std::size_t index = 0; index < communicators_.size(); ++index) {
        if (!communicators_[index]->start()) {
            rollbackStart(index);
            return {false, StartupStage::TurbineCommunicator,
                    "failed to start IEC communicator for turbine " +
                        std::to_string(index + 1)};
        }
        communicatorStartCount_ = index + 1;
    }

    started_ = true;
    return {true, StartupStage::None, {}};
}

void CommunicationOrchestrator::stop()
{
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    rollbackStart(communicatorStartCount_);
}

void CommunicationOrchestrator::rollbackStart(std::size_t communicatorCount)
{
    const std::size_t count = std::min(communicatorCount, communicators_.size());
    for (std::size_t index = count; index > 0; --index) {
        communicators_[index - 1]->stop();
    }
    communicatorStartCount_ = 0;

    if (iecStarted_ || initialized_) {
        iecWrapper_.stop();
        iecStarted_ = false;
        initialized_ = false;
    }
    iecStatus_.store(COMM_DISCONNECTED);

    if (dataHistorianStarted_) {
        socketWrapper_.StopDataHistorianServer();
        dataHistorianStarted_ = false;
    }
    if (attackStarted_) {
        socketWrapper_.StopAttackInterfaceServer();
        attackStarted_ = false;
    }
    if (operatorStarted_) {
        socketWrapper_.StopOperatorServer();
        operatorStarted_ = false;
    }
    socketStatus_.store(COMM_DISCONNECTED);
    started_ = false;
}

void CommunicationOrchestrator::setFailureHandler(FailureHandler handler)
{
    std::lock_guard<std::mutex> lock(failureHandlerMutex_);
    failureHandler_ = std::move(handler);
}

void CommunicationOrchestrator::handleRuntimeFailure(const std::string& message)
{
    if (message.rfind("IEC ", 0) == 0) {
        iecStatus_.store(COMM_DISCONNECTED);
    } else {
        socketStatus_.store(COMM_DISCONNECTED);
    }
    FailureHandler handler;
    {
        std::lock_guard<std::mutex> lock(failureHandlerMutex_);
        handler = failureHandler_;
    }
    if (handler) {
        handler(message);
    }
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
    if (iecStatus_.load() == COMM_DISCONNECTED) {
        return COMM_DISCONNECTED;
    }
    const IecConnectionStatus status = iecWrapper_.connectionStatus();
    if (status == IEC_LINK_CONNECTED) {
        return COMM_CONNECTED;
    }
    if (status == IEC_LINK_ERROR || status == IEC_LINK_CLOSED) {
        return COMM_DISCONNECTED;
    }
    return COMM_CONNECTING;
}
