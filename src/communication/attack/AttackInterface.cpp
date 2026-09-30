#include "sc/communication/attack/AttackInterface.hpp"

#include "sc/model/SharedData.hpp"
#include "sc/runtime/Logging.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <utility>

namespace AttackInterface {

AttackInterface::AttackInterface(int numTurbines,
                                 sc::ports::AttackChannel& channel,
                                 sc::ports::Clock& clock,
                                 AttackTiming timing)
    : numTurbines_(numTurbines),
      channel_(channel),
      clock_(clock),
      timing_(timing),
      sessionManager_(numTurbines, supportedSignalTypes(), clock, timing.sessionLeaseTimeout) {
    publishResourceUsage();
    channel_.setReceiveHandler([this](const uint8_t* data, std::size_t length) { handleMessage(data, length); });
    channel_.setLeaseCheckHandler([this]() { checkSessionLease(); });
    channel_.setDisconnectHandler([this](const std::string& reason) { endSession(reason); });
}

void AttackInterface::setAuditCallback(AuditCallback callback) {
    std::lock_guard<std::mutex> lock(auditMutex_);
    auditCallback_ = std::move(callback);
}

void AttackInterface::resetState() { endSession("reset"); }

void AttackInterface::shutdown(const std::string& reason) { endSession(reason); }

void AttackInterface::checkSessionLease() {
    const auto expired = sessionManager_.expireSession();
    if (!expired) {
        return;
    }

    cancelPendingOverwrite();
    attackStarted_.store(false);
    publishResourceUsage();
    audit(*expired, "disconnected");
}

void AttackInterface::txData(unsigned int turbineId, SignalType signalType, float value) {
    std::lock_guard<std::mutex> lock(operationMutex_);
    txDataUnlocked(turbineId, signalType, value);
}

AIRC AttackInterface::overwrite(unsigned int turbineId, SignalType signalType, float& value) {
    std::lock_guard<std::mutex> lock(operationMutex_);
    return overwriteUnlocked(turbineId, signalType, value);
}

AIRC AttackInterface::processValue(unsigned int turbineId, SignalType signalType, float& value) {
    std::lock_guard<std::mutex> lock(operationMutex_);
    txDataUnlocked(turbineId, signalType, value);
    return overwriteUnlocked(turbineId, signalType, value);
}

void AttackInterface::processValue(unsigned int turbineId, SignalType signalType, uint32_t value) {
    std::lock_guard<std::mutex> lock(operationMutex_);
    txDataUnlocked(turbineId, signalType, static_cast<float>(value));
}

void AttackInterface::txDataUnlocked(unsigned int turbineId, SignalType signalType, float value) {
    if (signalType == SignalType::NONE) {
        return;
    }

    if (!validTurbine(turbineId)) {
        ATTACK_ERR("Invalid turbine ID: " << turbineId);
        return;
    }

    if (!sessionManager_.tapEnabled(static_cast<int>(turbineId), signalType)) {
        return;
    }

    const auto message = sc::protocol::attack::encode(TxDataMessage{static_cast<uint8_t>(turbineId), signalType, 1, value});
    channel_.send(message.data(), message.size());
}

AIRC AttackInterface::overwriteUnlocked(unsigned int turbineId, SignalType signalType, float& value) {
    if (signalType == SignalType::NONE) return AI_DISABLED;
    if (!validTurbine(turbineId)) return AI_ERROR;
    if (!sessionManager_.fdiEnabled(static_cast<int>(turbineId), signalType)) return AI_DISABLED;

    const auto replacement = requestReplacement(turbineId, signalType);
    if (replacement && std::isfinite(*replacement) &&
        sessionManager_.setFdiValue(static_cast<int>(turbineId), signalType, *replacement)) {
        value = *replacement;
        return AI_OK;
    }

    if (timing_.reuseLastFdiValueOnFailure) {
        if (const auto cached = sessionManager_.fdiValue(
                static_cast<int>(turbineId), signalType)) {
            value = *cached;
            return AI_OK;
        }
    }
    return AI_TIMEOUT;
}

std::vector<SignalType> AttackInterface::supportedSignalTypes() {
    return {
        SignalType::WIND_SPEED,
        SignalType::WIND_DIRECTION,
        SignalType::TURBINE_STATUS,
        SignalType::POWER,
        SignalType::YAW_ANGLE,
        SignalType::ROTOR_SPEED,
        SignalType::PITCH_ANGLE,
        SignalType::YAW_SETPOINT,
        SignalType::POWER_SETPOINT,
        SignalType::GENERATOR_TORQUE,
        SignalType::OPERATION_COMMAND,
    };
}

std::string AttackInterface::signalTypeName(SignalType signalType) {
    switch (signalType) {
        case SignalType::WIND_SPEED: return "Wind speed";
        case SignalType::WIND_DIRECTION: return "Wind direction";
        case SignalType::TURBINE_STATUS: return "Turbine status";
        case SignalType::POWER: return "Power";
        case SignalType::YAW_ANGLE: return "Yaw angle";
        case SignalType::ROTOR_SPEED: return "Rotor speed";
        case SignalType::PITCH_ANGLE: return "Pitch angle";
        case SignalType::YAW_SETPOINT: return "Yaw setpoint";
        case SignalType::POWER_SETPOINT: return "Power setpoint";
        case SignalType::GENERATOR_TORQUE: return "Generator torque";
        case SignalType::OPERATION_COMMAND: return "Operation command";
        case SignalType::NONE: return "None";
        case SignalType::ARRAY: return "Array";
    }
    return "Unknown";
}

bool AttackInterface::validTurbine(unsigned int turbineId) const {
    return turbineId >= 1 && turbineId <= static_cast<unsigned int>(numTurbines_);
}

void AttackInterface::publishResourceUsage() const {
    int tapEnabled = 0;
    int fdiEnabled = 0;
    std::set<std::string> fdiSignals;
    const auto signalTypes = supportedSignalTypes();
    for (int turbineId = 1; turbineId <= numTurbines_; ++turbineId) {
        for (SignalType signalType : signalTypes) {
            if (sessionManager_.tapEnabled(turbineId, signalType)) ++tapEnabled;
            if (sessionManager_.fdiEnabled(turbineId, signalType)) {
                ++fdiEnabled;
                fdiSignals.insert(signalTypeName(signalType));
            }
        }
    }

    const int available = numTurbines_ * static_cast<int>(signalTypes.size());
    auto& interface = SharedData::instance().interface;
    std::lock_guard<std::mutex> lock(interface.mutex);
    interface.attackTapEnabled = tapEnabled;
    interface.attackTapAvailable = available;
    interface.attackFdiEnabled = fdiEnabled;
    interface.attackFdiAvailable = available;
    interface.attackFdiSignals.assign(fdiSignals.begin(), fdiSignals.end());
}

void AttackInterface::parseControl(const CtDataMessage& message) {
    const auto session = sessionManager_.session();
    if (!session) {
        ATTACK_ERR("Ignoring CT_DATA without an active configured session");
        return;
    }

    if (message.signal != ControlSignal::TAP && message.signal != ControlSignal::FDI) {
        protocolError("unsupported control signal");
        return;
    }

    bool anyEnabled = false;
    for (int turbineId = 1; turbineId <= numTurbines_; ++turbineId) {
        const bool value = message.enabled[static_cast<size_t>(turbineId - 1)] != 0;
        const bool updated = message.signal == ControlSignal::TAP
            ? sessionManager_.setTapEnabled(turbineId, message.dataType, value)
            : sessionManager_.setFdiEnabled(turbineId, message.dataType, value);
        if (!updated) {
            protocolError("unsupported attack signal type");
            return;
        }
        anyEnabled = anyEnabled || value;
        audit(*session,
              std::string(message.signal == ControlSignal::TAP ? "tap" : "fdi") +
                  ";signal=" + signalTypeName(message.dataType) +
                  ";turbine=" + std::to_string(turbineId) +
                  ";enabled=" + (value ? "true" : "false"));
    }
    publishResourceUsage();
    if (anyEnabled && !attackStarted_.exchange(true)) {
        audit(*session, "attack_started");
    }
}

void AttackInterface::parseAttackData(const AtDataMessage& message) {
    std::lock_guard<std::mutex> lock(requestMutex_);
    if (awaitingResponse_ &&
        message.dataType == requestedSignalType_ &&
        message.turbineId == requestedTurbineId_ &&
        message.attackTime == requestedAt_) {
        responseValue_ = message.fakeValue;
        awaitingResponse_ = false;
        requestCondition_.notify_all();
    }
}

void AttackInterface::parseConfiguration(const CfgDataMessage& configuration) {
    endSession("reconfigured");
    const auto started = sessionManager_.startSession(configuration.teamName);
    if (!started) {
        ATTACK_ERR("Rejected attack session: " << started.error);
        return;
    }
    attackStarted_.store(false);
    const auto session = sessionManager_.session();
    if (session) audit(*session, "connected");
    publishResourceUsage();

    const auto acknowledgement = sc::protocol::attack::encode(configuration);
    if (!channel_.send(acknowledgement.data(), acknowledgement.size())) {
        endSession("configuration acknowledgement failed");
    }
}

void AttackInterface::handleMessage(const uint8_t* data, std::size_t length) {
    if (data == nullptr || length == 0) return;
    try {
        const auto message = sc::protocol::attack::decode(data, length, static_cast<size_t>(numTurbines_));
        std::visit([this](const auto& value) { handleDecodedMessage(value); }, message);
    } catch (const sc::protocol::attack::ProtocolError& error) {
        protocolError(error.what());
    }
}

void AttackInterface::handleDecodedMessage(const CtDataMessage& message) { parseControl(message); }
void AttackInterface::handleDecodedMessage(const AtDataMessage& message) { parseAttackData(message); }
void AttackInterface::handleDecodedMessage(const CfgDataMessage& message) { parseConfiguration(message); }
void AttackInterface::handleDecodedMessage(const HeartbeatMessage&) { sessionManager_.heartbeat(); }
void AttackInterface::handleDecodedMessage(const ReleaseMessage&) { endSession("client release"); }
void AttackInterface::handleDecodedMessage(const SimCtrlMessage&) { ATTACK_LOG_V1("Ignoring unused SIM_CTRL command"); }
void AttackInterface::handleDecodedMessage(const TxDataMessage&) { protocolError("unexpected TX_DATA message"); }
void AttackInterface::handleDecodedMessage(const RqDataMessage&) { protocolError("unexpected RQ_DATA message"); }

void AttackInterface::protocolError(const std::string& message) {
    ATTACK_ERR(message);
    endSession("protocol error");
}

void AttackInterface::cancelPendingOverwrite() {
    std::lock_guard<std::mutex> lock(requestMutex_);
    awaitingResponse_ = false;
    responseValue_.reset();
    requestCondition_.notify_all();
}

std::optional<float> AttackInterface::requestReplacement(unsigned int turbineId, SignalType signalType) {
    TimeStamp requestTime = 0;
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        requestTime = std::max(clock_.unixTimeMilliseconds(), lastRequestTime_ + 1);
        lastRequestTime_ = requestTime;
        awaitingResponse_ = true;
        responseValue_.reset();
        requestedSignalType_ = signalType;
        requestedTurbineId_ = static_cast<int>(turbineId);
        requestedAt_ = requestTime;
    }

    const auto request = sc::protocol::attack::encode(RqDataMessage{
        static_cast<uint8_t>(turbineId),
        signalType,
        requestTime,
        requestTime + static_cast<TimeStamp>(timing_.requestLifetime.count())});
    if (!channel_.send(request.data(), request.size())) {
        cancelPendingOverwrite();
        return std::nullopt;
    }

    std::unique_lock<std::mutex> lock(requestMutex_);
    const bool completed = requestCondition_.wait_for(
        lock, timing_.requestLifetime, [this]() { return !awaitingResponse_; });
    if (!completed) {
        awaitingResponse_ = false;
        responseValue_.reset();
        return std::nullopt;
    }
    const auto response = responseValue_;
    responseValue_.reset();
    return response;
}

void AttackInterface::endSession(const std::string& reason) {
    const auto closed = sessionManager_.endSession(reason);
    if (!closed) return;
    cancelPendingOverwrite();
    attackStarted_.store(false);
    publishResourceUsage();
    audit(*closed, "disconnected");
}

void AttackInterface::audit(const sc::application::AttackSessionInfo& session, const std::string& event) const {
    emitAudit(session.id, session.label, event);
}

void AttackInterface::audit(const sc::application::ClosedAttackSession& session, const std::string& event) const {
    emitAudit(session.id, session.label, event + ";reason=" + session.reason);
}

void AttackInterface::emitAudit(sc::application::AttackSessionId id,
                                const std::string& label,
                                const std::string& event) const {
    std::ostringstream output;
    output << "timestamp_ms=" << clock_.unixTimeMilliseconds()
           << ";session=" << id
           << ";label=" << std::quoted(label)
           << ";event=" << event;
    const std::string message = output.str();
    ATTACK_ST(message);
    AuditCallback callback;
    {
        std::lock_guard<std::mutex> lock(auditMutex_);
        callback = auditCallback_;
    }
    if (callback) callback(message);
}

} // namespace AttackInterface
