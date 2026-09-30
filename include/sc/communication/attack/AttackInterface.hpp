#pragma once

#include "sc/communication/attack/AttackChannel.hpp"
#include "sc/communication/attack/AttackProtocol.hpp"
#include "sc/communication/attack/AttackSignalType.hpp"
#include "sc/runtime/Clock.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

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
    std::chrono::milliseconds requestRetryPeriod{500};
    std::chrono::milliseconds sessionLeaseTimeout{750};
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
    AttackInterface(int numTurbines, sc::ports::AttackChannel& channel, sc::ports::Clock& clock = sc::ports::systemClock(), AttackTiming timing = {});
    ~AttackInterface();

    AttackInterface(const AttackInterface&) = delete;
    AttackInterface& operator=(const AttackInterface&) = delete;

    void setAuditCallback(AuditCallback callback);
    void resetState();
    void shutdown(const std::string& reason = "controller shutdown");
    void checkSessionLease();
    void txData(unsigned int turbineId, SignalType signalType, void* value);
    AIRC overwrite(unsigned int turbineId, SignalType signalType, float& value);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace AttackInterface
