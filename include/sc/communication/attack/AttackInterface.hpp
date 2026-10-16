#pragma once

#include "sc/communication/attack/AttackChannel.hpp"
#include "sc/communication/attack/AttackProtocol.hpp"
#include "sc/communication/attack/AttackSessionManager.hpp"
#include "sc/communication/attack/AttackSignalType.hpp"
#include "sc/runtime/Clock.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace AttackInterface {

using MessageType = sc::protocol::attack::MessageType;
using ControlSignal = sc::protocol::attack::ControlSignal;
using TimeStamp = sc::protocol::attack::TimeStamp;
using TxDataMessage = sc::protocol::attack::TxDataMessage;
using RqDataMessage = sc::protocol::attack::RqDataMessage;
using AtDataMessage = sc::protocol::attack::AtDataMessage;
using CtDataMessage = sc::protocol::attack::CtDataMessage;
using CfgDataMessage = sc::protocol::attack::CfgDataMessage;
using SimCtrlMessage = sc::protocol::attack::SimCtrlMessage;
using HeartbeatMessage = sc::protocol::attack::HeartbeatMessage;
using ReleaseMessage = sc::protocol::attack::ReleaseMessage;

struct AttackTiming {
    std::chrono::milliseconds requestLifetime{250};
    std::chrono::milliseconds sessionLeaseTimeout{sc::protocol::attack::DEFAULT_HEARTBEAT_TIMEOUT};
    bool reuseLastFdiValueOnFailure{true};
};

enum AIRC {
    AI_OK = 1,
    AI_DISABLED = 0,
    AI_ERROR = -1,
    AI_TIMEOUT = -2,
};

using AuditCallback = std::function<void(const std::string&)>;

class AttackInterface {
    public:
    AttackInterface(int numTurbines, sc::ports::AttackChannel& channel, sc::ports::Clock& clock = sc::ports::systemClock(),
                    AttackTiming timing = {});

    AttackInterface(const AttackInterface&) = delete;
    AttackInterface& operator=(const AttackInterface&) = delete;

    void setAuditCallback(AuditCallback callback);
    void resetState();
    void shutdown(const std::string& reason = "controller shutdown");
    void checkSessionLease();
    void txData(unsigned int turbineId, SignalType signalType, float value);
    void txData(unsigned int turbineId, SignalType signalType, uint32_t value);
    AIRC overwrite(unsigned int turbineId, SignalType signalType, float& value);
    AIRC processValue(unsigned int turbineId, SignalType signalType, float& value);
    void processValue(unsigned int turbineId, SignalType signalType, uint32_t value);

    private:
    void txDataUnlocked(unsigned int turbineId, SignalType signalType, float value);
    void txDataUnlocked(unsigned int turbineId, SignalType signalType, uint32_t value);
    AIRC overwriteUnlocked(unsigned int turbineId, SignalType signalType, float& value);
    static std::vector<SignalType> supportedSignalTypes();
    static std::string signalTypeName(SignalType signalType);
    bool validTurbine(unsigned int turbineId) const;
    void publishResourceUsage() const;
    void parseControl(const CtDataMessage& message);
    void parseAttackData(const AtDataMessage& message);
    void parseConfiguration(const CfgDataMessage& configuration);
    void handleMessage(const uint8_t* data, std::size_t length);
    void handleDecodedMessage(const CtDataMessage& message);
    void handleDecodedMessage(const AtDataMessage& message);
    void handleDecodedMessage(const CfgDataMessage& message);
    void handleDecodedMessage(const HeartbeatMessage& message);
    void handleDecodedMessage(const ReleaseMessage& message);
    void handleDecodedMessage(const SimCtrlMessage& message);
    void handleDecodedMessage(const TxDataMessage& message);
    void handleDecodedMessage(const RqDataMessage& message);
    void protocolError(const std::string& message);
    void cancelPendingOverwrite();
    std::optional<float> requestReplacement(unsigned int turbineId, SignalType signalType);
    void endSession(const std::string& reason);
    void audit(const sc::application::AttackSessionInfo& session, const std::string& event) const;
    void audit(const sc::application::ClosedAttackSession& session, const std::string& event) const;
    void emitAudit(sc::application::AttackSessionId id, const std::string& label, const std::string& event) const;

    int numTurbines_;
    sc::ports::AttackChannel& channel_;
    sc::ports::Clock& clock_;
    AttackTiming timing_;
    sc::application::AttackSessionManager sessionManager_;

    mutable std::mutex auditMutex_;
    AuditCallback auditCallback_;
    std::atomic<bool> attackStarted_{false};

    std::mutex operationMutex_;
    std::mutex requestMutex_;
    std::condition_variable requestCondition_;
    bool awaitingResponse_{false};
    SignalType requestedSignalType_{SignalType::NONE};
    int requestedTurbineId_{0};
    TimeStamp requestedAt_{0};
    TimeStamp lastRequestTime_{0};
    std::optional<float> responseValue_;
};

} // namespace AttackInterface
