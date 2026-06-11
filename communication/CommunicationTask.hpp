#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/GlobalDataStructure.hpp"
#include "common/DataHistorian.hpp"
#include "communication/CommunicationTypes.hpp"
#include "communication/socket/SocketWrapper.hpp"
#include "communication/AttackInterface.hpp"

class IECCommunicator;

/**
 * Owns the communication subsystem lifecycle.
 *
 * The orchestrator binds socket callbacks, starts the IEC wrapper, and creates
 * one IECCommunicator per configured turbine. It is intentionally the only
 * class that owns the shared AttackInterface mutex so RX/TX paths serialize
 * requests consistently.
 */
class CommunicationOrchestrator {
public:
    explicit CommunicationOrchestrator(const CommConfig& config = CommConfig{});
    ~CommunicationOrchestrator();

    void init();
    bool start();
    void stop();

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

    CommConfig config_;
    LibIecWrapper iecWrapper_;
    SocketWrapper socketWrapper_;
    AttackInterface::Controller attackInterface_;
    std::mutex attackInterfaceMutex_;
    std::vector<std::unique_ptr<IECCommunicator>> communicators_;
    std::atomic<CommStatus> socketStatus_{COMM_DISCONNECTED};
    std::atomic<CommStatus> iecStatus_{COMM_DISCONNECTED};
};
