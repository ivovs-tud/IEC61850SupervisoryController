#include "CommunicationTask.hpp"
#include "IECCommunicator.hpp"
#include "SocketWrapper.hpp"
#include "AttackInterface.hpp"
#include "config.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace {

AttackInterface::AttackTiming makeAttackTiming(const CommConfig& config)
{
    AttackInterface::AttackTiming timing;
    timing.sessionLeaseTimeout = config.attackInterface.leaseTimeout;
    return timing;
}

AttackChannelTCP::Config makeTcpAttackConfig(const CommConfig& config) {
    AttackChannelTCP::Config tcp;
    tcp.bindAddress = config.attackInterface.bindAddress;
    tcp.port = config.attackInterface.port;
    tcp.pollPeriod = config.attackInterface.pollPeriod;
    tcp.turbineCount = config.mms.turbines.size();
    tcp.receiveBufferBytes = config.attackInterface.receiveBufferBytes;
    tcp.transmitBufferBytes = config.attackInterface.transmitBufferBytes;
    tcp.tcpUserTimeout = config.attackInterface.tcpUserTimeout;
    return tcp;
}

} // namespace

CommunicationOrchestrator::CommunicationOrchestrator(const CommConfig& config)
    : config_(config),
      iecManager_(sc::ports::systemClock(),
                  config.mms.reconnectInitialDelay,
                  config.mms.reconnectMaxDelay),
      socketWrapper_(config.operatorServer.port,
                     static_cast<int>(config.operatorServer.pollPeriod.count()),
                     config.attackInterface.port,
                     static_cast<int>(config.attackInterface.pollPeriod.count()),
                     config.dataHistorian.port,
                     static_cast<int>(config.dataHistorian.pollPeriod.count())),
      tcpAttackChannel_(makeTcpAttackConfig(config)),
      attackChannel_(config.attackInterface.transport == sc::ports::AttackTransport::TCP
                         ? static_cast<sc::ports::AttackChannel&>(tcpAttackChannel_)
                         : static_cast<sc::ports::AttackChannel&>(socketWrapper_)),
      attackInterface_(static_cast<int>(config.mms.turbines.size()),
                       attackChannel_,
                       sc::ports::systemClock(),
                       makeAttackTiming(config))
{
    socketStatus_.store(COMM_DISCONNECTED);
    iecStatus_.store(COMM_DISCONNECTED);
    socketWrapper_.ConfigureAttackInterface(
        config.attackInterface.receiveBufferBytes,
        config.attackInterface.transmitBufferBytes,
        config.attackInterface.zmqHeartbeatInterval,
        config.attackInterface.zmqHeartbeatTimeout);
    socketWrapper_.setFailureHandler([this](const std::string& message) {
        if (message.rfind("attack interface server:", 0) == 0) {
            attackInterface_.shutdown("transport failure");
        }
        handleRuntimeFailure(message);
    });
    tcpAttackChannel_.setFailureHandler([this](const std::string& message) {
        attackInterface_.shutdown("transport failure");
        handleRuntimeFailure("raw TCP attack interface server: " + message);
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
    if (config_.mms.turbines.empty()) {
        return {false, StartupStage::Initialization, "at least one MMS turbine is required"};
    }
    for (std::size_t index = 0; index < config_.mms.turbines.size(); ++index) {
        const auto& endpoint = config_.mms.turbines[index];
        iecManager_.addTurbine(static_cast<int>(index) + 1,
                               endpoint.host,
                               endpoint.port,
                               endpoint.logicalDevice,
                               endpoint.iedName);
        for (const auto& reference : endpoint.gooseRefs) {
            const int turbineId = static_cast<int>(index) + 1;
            if (!iecManager_.addGooseSubscription(
                    turbineId, reference,
                    [turbineId](const std::string& receivedReference, int32_t value) {
                        (void)turbineId;
                        (void)receivedReference;
                        (void)value;
                        COMMTASK_LOG_V2("GOOSE turbine " << turbineId << " "
                                         << receivedReference << "=" << value);
                    })) {
                return {false, StartupStage::Initialization,
                        "failed to configure GOOSE subscription for turbine " +
                            std::to_string(turbineId)};
            }
        }
    }
    if (config_.goose.enabled &&
        !iecManager_.configureGoose(config_.goose.networkInterface)) {
        return {false, StartupStage::Initialization, "failed to configure GOOSE receiver"};
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

    attackInterface_.setAuditCallback([](const std::string& event) {
        DataHistorian::instance().log("[AttackInterface] " + event);
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
            config_, turbineId, iecManager_, attackInterface_, attackInterfaceMutex_);
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
    const bool attackServerStarted =
        config_.attackInterface.transport == sc::ports::AttackTransport::TCP
            ? tcpAttackChannel_.start()
            : socketWrapper_.StartAttackInterfaceServer(config_.attackInterface.port) >= tcpSOCKET_CONNECTED;
    if (!attackServerStarted) {
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
    if (config_.goose.enabled) {
        if (!iecManager_.startGoose()) {
            rollbackStart(0);
            return {false, StartupStage::Iec, "failed to start GOOSE receiver"};
        }
        gooseStarted_ = true;
    }
    for (std::size_t index = 0; index < communicators_.size(); ++index) {
        if (!communicators_[index]->start()) {
            rollbackStart(index);
            return {false, StartupStage::TurbineCommunicator,
                    "failed to start IEC communicator for turbine " +
                        std::to_string(index + 1)};
        }
        communicatorStartCount_ = index + 1;
    }

    iecStatus_.store(
        iecManager_.status() == IEC_LINK_CONNECTED ? COMM_CONNECTED : COMM_CONNECTING);

    {
        auto& interface = SharedData::instance().interface;
        std::lock_guard<std::mutex> interfaceLock(interface.mutex);
        interface.systemRunning = true;
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

    if (gooseStarted_) {
        iecManager_.stopGoose();
        gooseStarted_ = false;
    }
    iecManager_.disconnectAll();
    iecStatus_.store(COMM_DISCONNECTED);
    {
        auto& interface = SharedData::instance().interface;
        std::lock_guard<std::mutex> interfaceLock(interface.mutex);
        interface.systemRunning = false;
    }

    if (dataHistorianStarted_) {
        socketWrapper_.StopDataHistorianServer();
        dataHistorianStarted_ = false;
    }
    if (attackStarted_) {
        attackInterface_.shutdown("controller shutdown");
        if (config_.attackInterface.transport == sc::ports::AttackTransport::TCP) {
            tcpAttackChannel_.stop();
        } else {
            socketWrapper_.StopAttackInterfaceServer();
        }
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
    const IecConnectionStatus status = iecManager_.status();
    if (status == IEC_LINK_CONNECTED) {
        return COMM_CONNECTED;
    }
    if (status == IEC_LINK_ERROR || status == IEC_LINK_CLOSED) {
        return COMM_DISCONNECTED;
    }
    return COMM_CONNECTING;
}
