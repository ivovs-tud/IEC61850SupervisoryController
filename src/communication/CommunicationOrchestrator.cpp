#include "sc/communication/CommunicationOrchestrator.hpp"
#include "sc/communication/iec61850/IECCommunicator.hpp"
#include "sc/communication/attack/AttackInterface.hpp"
#include "sc/runtime/Logging.hpp"
#include "sc/runtime/Time.hpp"

#include <algorithm>
#include <cstdio>
#include <type_traits>
#include <utility>

namespace {

AttackInterface::AttackTiming makeAttackTiming(const CommunicationConfig& config)
{
    AttackInterface::AttackTiming timing;
    timing.sessionLeaseTimeout = config.attackInterface.leaseTimeout;
    return timing;
}

AttackChannelTCP::Config makeTcpAttackConfig(const CommunicationConfig& config) {
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

AttackChannelZMQ::Config makeZmqAttackConfig(const CommunicationConfig& config) {
    AttackChannelZMQ::Config zmq;
    zmq.port = config.attackInterface.port;
    zmq.pollPeriod = config.attackInterface.pollPeriod;
    zmq.receiveBufferBytes = config.attackInterface.receiveBufferBytes;
    zmq.transmitBufferBytes = config.attackInterface.transmitBufferBytes;
    zmq.heartbeatInterval = config.attackInterface.zmqHeartbeatInterval;
    zmq.heartbeatTimeout = config.attackInterface.zmqHeartbeatTimeout;
    return zmq;
}

OperatorServer::Config makeOperatorConfig(const CommunicationConfig& config) {
    return {config.operatorServer.port, config.operatorServer.pollPeriod};
}

DataHistorianServer::Config makeDataHistorianConfig(const CommunicationConfig& config) {
    return {config.dataHistorian.port, config.dataHistorian.pollPeriod};
}

HmiConfig makeHmiConfig(const CommunicationConfig& config) {
    HmiConfig hmi = defaultHmiConfig(static_cast<int>(config.mms.turbines.size()));
    hmi.windowSize = config.hmi.windowSize;
    hmi.publisherEndpoint = config.hmi.publisherEndpoint;
    hmi.commandEndpoint = config.hmi.commandEndpoint;
    hmi.alarmAcknowledgementEnabled = config.hmi.alarmAcknowledgementEnabled;
    return hmi;
}

} // namespace

CommunicationOrchestrator::CommunicationOrchestrator(const CommunicationConfig& config)
    : config_(config),
      hmiInterface_(makeHmiConfig(config), config.hmi.period),
      iecManager_(sc::ports::systemClock(), config.mms.reconnectInitialDelay, config.mms.reconnectMaxDelay),
      operatorServer_(makeOperatorConfig(config)),
      tcpAttackChannel_(makeTcpAttackConfig(config)),
      zmqAttackChannel_(makeZmqAttackConfig(config)),
      attackChannel_(config.attackInterface.transport == sc::ports::AttackTransport::TCP
                         ? static_cast<sc::ports::AttackChannel&>(tcpAttackChannel_)
                         : static_cast<sc::ports::AttackChannel&>(zmqAttackChannel_)),
      attackInterface_(static_cast<int>(config.mms.turbines.size()),
                       attackChannel_,
                       sc::ports::systemClock(),
                       makeAttackTiming(config)),
      dataHistorianServer_(makeDataHistorianConfig(config))
{
    socketStatus_.store(COMM_DISCONNECTED);
    iecStatus_.store(COMM_DISCONNECTED);
    operatorServer_.setFailureHandler([this](const std::string& message) {
        handleRuntimeFailure("operator server: " + message);
    });
    zmqAttackChannel_.setFailureHandler([this](const std::string& message) {
        attackInterface_.shutdown("transport failure");
        handleRuntimeFailure("ZeroMQ attack interface server: " + message);
    });
    tcpAttackChannel_.setFailureHandler([this](const std::string& message) {
        attackInterface_.shutdown("transport failure");
        handleRuntimeFailure("raw TCP attack interface server: " + message);
    });
    dataHistorianServer_.setFailureHandler([this](const std::string& message) {
        handleRuntimeFailure("data historian server: " + message);
    });
    hmiInterface_.setFailureHandler([](const std::string& message) {
        COMMTASK_ERR("Optional HMI interface stopped after failure: " << message);
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

    operatorServer_.setCommandHandler([this](const OperatorCommand& command) {
        std::visit([this](const auto& value) {
            using Command = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Command, RequestedPowerCommand>) {
                {
                    auto& control = SharedData::instance().control;
                    std::lock_guard<std::mutex> lock(control.mutex);
                    control.requestedPower = value.value;
                }
                COMMTASK_LOG_V1("Updated RequestedReferencePower to " << value.value);
            } else if constexpr (std::is_same_v<Command, SimulationStateCommand>) {
                {
                    auto& interface = SharedData::instance().interface;
                    std::lock_guard<std::mutex> lock(interface.mutex);
                    interface.simStarted = value.running;
                    if (!value.running) {
                        interface.simConfigured = false;
                    }
                }
                if (!value.running) {
                    DataHistorian::instance().stopRun();
                }
                COMMTASK_LOG_V1("Received simulation control message from operator server: simStarting = " << value.running);
            }
        }, command);
    });

    dataHistorianServer_.setRecordHandler([](const DataHistorianRecord& record) {
        char logMsg[512];
        snprintf(logMsg, sizeof(logMsg), "[WT%u]%llu;YawAng=%.1f;YawSpt=%.1f;W=%.1f;WSpt=%.1f;V=%.1f;D=%.1f;RotSpd=%.1f;Pth=%.1f;PthSpt=%.1f",
            record.turbineId, static_cast<unsigned long long>(record.unixTime),
            record.yawAngle, record.yawSetpoint,
            record.power, record.powerSetpoint, record.windSpeed, record.windDirection,
            record.rotorSpeed, record.pitchAngle, record.pitchSetpoint);

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
    if (!hmiInterface_.start()) {
        COMMTASK_ERR("HMI unavailable; controller will continue without it: "
                     << hmiInterface_.failureMessage());
    } else {
        hmiStarted_ = true;
    }
    if (!operatorServer_.start()) {
        COMMTASK_ERR("Failed to start operator server on port " << config_.operatorServer.port);
        rollbackStart(0);
        return {false, StartupStage::OperatorServer, "failed to start operator server"};
    }
    operatorStarted_ = true;
    const bool attackServerStarted =
        config_.attackInterface.transport == sc::ports::AttackTransport::TCP
            ? tcpAttackChannel_.start()
            : zmqAttackChannel_.start();
    if (!attackServerStarted) {
        COMMTASK_ERR("Failed to start attack interface server on port " << config_.attackInterface.port);
        rollbackStart(0);
        return {false, StartupStage::AttackInterface, "failed to start attack interface server"};
    }
    attackStarted_ = true;
    if (!dataHistorianServer_.start()) {
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
        dataHistorianServer_.stop();
        dataHistorianStarted_ = false;
    }
    if (attackStarted_) {
        attackInterface_.shutdown("controller shutdown");
        if (config_.attackInterface.transport == sc::ports::AttackTransport::TCP) {
            tcpAttackChannel_.stop();
        } else {
            zmqAttackChannel_.stop();
        }
        attackStarted_ = false;
    }
    if (operatorStarted_) {
        operatorServer_.stop();
        operatorStarted_ = false;
    }
    if (hmiStarted_) {
        hmiInterface_.stop();
        hmiStarted_ = false;
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

CommunicationStatus CommunicationOrchestrator::socketStatus() const
{
    return socketStatus_.load();
}

CommunicationStatus CommunicationOrchestrator::iecStatus() const
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
