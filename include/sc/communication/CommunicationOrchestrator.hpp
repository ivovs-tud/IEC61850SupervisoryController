#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "sc/model/SharedData.hpp"
#include "sc/runtime/DataHistorian.hpp"
#include "sc/communication/CommunicationConfig.hpp"
#include "sc/communication/attack/AttackChannelTCP.hpp"
#include "sc/communication/attack/AttackChannelZMQ.hpp"
#include "sc/communication/attack/AttackInterface.hpp"
#include "sc/communication/data_historian/DataHistorianServer.hpp"
#include "sc/communication/hmi/HmiInterface.hpp"
#include "sc/communication/iec61850/IEC61850Manager.hpp"
#include "sc/communication/operator/OperatorServer.hpp"

class IECCommunicator;

class CommunicationOrchestrator
{
public:
    enum class StartupStage {
        None,
        Initialization,
        OperatorServer,
        AttackInterface,
        DataHistorian,
        Iec,
        TurbineCommunicator,
        AlreadyRunning,
    };

    struct StartupResult {
        bool success{false};
        StartupStage stage{StartupStage::None};
        std::string message;

        explicit operator bool() const noexcept { return success; }
    };

    using FailureHandler = std::function<void(const std::string&)>;

    explicit CommunicationOrchestrator(const CommunicationConfig& config = CommunicationConfig{});
    ~CommunicationOrchestrator();

    StartupResult init();
    StartupResult start();
    void stop();
    void setFailureHandler(FailureHandler handler);

    struct CommunicatorState {
        int turbineId;
        CommunicationStatus iecStatus;
        std::chrono::system_clock::time_point lastActivityTime;
    };

    std::vector<CommunicatorState> communicatorStates() const;
    CommunicationStatus socketStatus() const;
    CommunicationStatus iecStatus() const;

private:
    void createCommunicators();
    void handleRuntimeFailure(const std::string& message);
    void rollbackStart(std::size_t communicatorCount);

    CommunicationConfig config_;
    HmiInterface hmiInterface_;
    IEC61850Manager iecManager_;
    OperatorServer operatorServer_;
    AttackChannelTCP tcpAttackChannel_;
    AttackChannelZMQ zmqAttackChannel_;
    sc::ports::AttackChannel& attackChannel_;
    AttackInterface::AttackInterface attackInterface_;
    DataHistorianServer dataHistorianServer_;
    std::mutex attackInterfaceMutex_;
    std::vector<std::unique_ptr<IECCommunicator>> communicators_;
    std::atomic<CommunicationStatus> socketStatus_{COMM_DISCONNECTED};
    std::atomic<CommunicationStatus> iecStatus_{COMM_DISCONNECTED};
    std::mutex lifecycleMutex_;
    std::mutex failureHandlerMutex_;
    FailureHandler failureHandler_;
    bool initialized_{false};
    bool started_{false};
    bool operatorStarted_{false};
    bool hmiStarted_{false};
    bool attackStarted_{false};
    bool dataHistorianStarted_{false};
    bool gooseStarted_{false};
    std::size_t communicatorStartCount_{0};
};
