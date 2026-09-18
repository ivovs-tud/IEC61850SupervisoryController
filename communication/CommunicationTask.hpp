#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/SharedData.hpp"
#include "common/DataHistorian.hpp"
#include "communication/CommunicationTypes.hpp"
#include "communication/socket/SocketWrapper.hpp"
#include "communication/AttackInterface.hpp"

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

    explicit CommunicationOrchestrator(const CommConfig& config = CommConfig{});
    ~CommunicationOrchestrator();

    StartupResult init();
    StartupResult start();
    void stop();
    void setFailureHandler(FailureHandler handler);

    struct CommunicatorState {
        int turbineId;
        CommStatus iecStatus;
        std::chrono::system_clock::time_point lastActivityTime;
    };

    std::vector<CommunicatorState> communicatorStates() const;
    CommStatus socketStatus() const;
    CommStatus iecStatus() const;

private:
    void createCommunicators();
    void handleRuntimeFailure(const std::string& message);
    void rollbackStart(std::size_t communicatorCount);

    CommConfig config_;
    libiec_wrapper iecWrapper_;
    SocketWrapper socketWrapper_;
    AttackInterface::AttackInterface attackInterface_;
    std::mutex attackInterfaceMutex_;
    std::vector<std::unique_ptr<IECCommunicator>> communicators_;
    std::atomic<CommStatus> socketStatus_{COMM_DISCONNECTED};
    std::atomic<CommStatus> iecStatus_{COMM_DISCONNECTED};
    std::mutex lifecycleMutex_;
    std::mutex failureHandlerMutex_;
    FailureHandler failureHandler_;
    bool initialized_{false};
    bool started_{false};
    bool operatorStarted_{false};
    bool attackStarted_{false};
    bool dataHistorianStarted_{false};
    bool iecStarted_{false};
    std::size_t communicatorStartCount_{0};
};
