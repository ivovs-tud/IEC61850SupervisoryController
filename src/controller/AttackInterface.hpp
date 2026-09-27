#pragma once

#include "SharedData.hpp"
#include "config.hpp"
#include "sc/application/AttackSessionManager.hpp"
#include "sc/ports/AttackChannel.hpp"
#include "sc/ports/Clock.hpp"
#include "sc/protocol/AttackProtocol.hpp"

#include <chrono>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <sstream>
#include <iomanip>
#include <string>
#include <utility>
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
    std::chrono::milliseconds requestRetryPeriod{500};
    std::chrono::milliseconds sessionLeaseTimeout{750};
};

typedef enum eAIRC {
    AI_OK = 1,
    AI_DISABLED = 0,
    AI_ERROR = -1,
    AI_TIMEOUT = -2
} AIRC;

using AuditCallback = std::function<void(const std::string&)>;

class AttackInterface {
public:
    AttackInterface(int numTurbines,
                    sc::ports::AttackChannel& channel,
                    sc::ports::Clock& clock = sc::ports::systemClock(),
                    AttackTiming timing = {})
        : numTurbines_(numTurbines),
          channel_(channel),
          clock_(clock),
          timing_(timing),
          sessionManager_(numTurbines, supportedSignalTypes(), clock, timing.sessionLeaseTimeout) {
        publishResourceUsage();
        channel_.setReceiveHandler([this](const uint8_t* data, size_t length) {
            handleMessage(data, length);
        });
        channel_.setLeaseCheckHandler([this]() { checkSessionLease(); });
        channel_.setDisconnectHandler([this](const std::string& reason) { endSession(reason); });
    }

    void setAuditCallback(AuditCallback callback) {
        std::lock_guard<std::mutex> lock(auditMutex_);
        auditCallback_ = std::move(callback);
    }

    void resetState() { endSession("reset"); }

    void shutdown(const std::string& reason = "controller shutdown") { endSession(reason); }

    void checkSessionLease() {
        const auto expired = sessionManager_.expireSession();
        if (!expired) return;
        cancelPendingOverwrite();
        attackStarted_.store(false);
        publishResourceUsage();
        audit(*expired, "disconnected");
    }

    void txData(unsigned int turbineId, SignalType signalType, void* value) {
        if (signalType == SignalType::NONE) return;
        if (!validTurbine(turbineId)) {
            ATTACK_ERR("Invalid turbine ID: " << turbineId);
            return;
        }
        if (!sessionManager_.tapEnabled(static_cast<int>(turbineId), signalType)) return;

        const auto message = sc::protocol::attack::encode(TxDataMessage{
            static_cast<uint8_t>(turbineId), signalType, 1, *static_cast<float*>(value)});
        channel_.send(message.data(), message.size());
    }

    AIRC overwrite(unsigned int turbineId, SignalType signalType, float& value) {
        if (signalType == SignalType::NONE) return AI_DISABLED;
        if (!validTurbine(turbineId)) return AI_ERROR;
        if (!sessionManager_.fdiEnabled(static_cast<int>(turbineId), signalType)) return AI_DISABLED;

        txData(turbineId, signalType, &value);
        if (const auto replacement = sessionManager_.fdiValue(
                static_cast<int>(turbineId), signalType)) {
            value = *replacement;
            return AI_OK;
        }

        requestReplacement(turbineId, signalType);
        if (const auto replacement = sessionManager_.fdiValue(
                static_cast<int>(turbineId), signalType)) {
            value = *replacement;
            return AI_OK;
        }
        return AI_TIMEOUT;
    }

private:
    static std::vector<SignalType> supportedSignalTypes() {
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

    static std::string signalTypeName(SignalType signalType) {
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

    bool validTurbine(unsigned int turbineId) const {
        return turbineId >= 1 && turbineId <= static_cast<unsigned int>(numTurbines_);
    }

    void publishResourceUsage() const {
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

    void parseControl(const CtDataMessage& message) {
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

    void parseAttackData(const AtDataMessage& message) {
        if (std::isnan(message.fakeValue) ||
            !sessionManager_.setFdiValue(static_cast<int>(message.turbineId), message.dataType, message.fakeValue)) {
            return;
        }
        std::lock_guard<std::mutex> lock(requestMutex_);
        if (message.dataType == requestedSignalType_ && message.turbineId == requestedTurbineId_) {
            awaitingResponse_ = false;
        }
    }

    void parseConfiguration(const CfgDataMessage& configuration) {
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

    void handleMessage(const uint8_t* data, size_t length) {
        if (data == nullptr || length == 0) return;
        try {
            const auto message = sc::protocol::attack::decode(data, length, static_cast<size_t>(numTurbines_));
            std::visit([this](const auto& value) { handleDecodedMessage(value); }, message);
        } catch (const sc::protocol::attack::ProtocolError& error) {
            protocolError(error.what());
        }
    }

    void handleDecodedMessage(const CtDataMessage& message) { parseControl(message); }
    void handleDecodedMessage(const AtDataMessage& message) { parseAttackData(message); }
    void handleDecodedMessage(const CfgDataMessage& message) { parseConfiguration(message); }
    void handleDecodedMessage(const HeartbeatMessage&) { sessionManager_.heartbeat(); }
    void handleDecodedMessage(const ReleaseMessage&) { endSession("client release"); }
    void handleDecodedMessage(const SimCtrlMessage&) { ATTACK_LOG_V1("Ignoring unused SIM_CTRL command"); }
    void handleDecodedMessage(const TxDataMessage&) { protocolError("unexpected TX_DATA message"); }
    void handleDecodedMessage(const RqDataMessage&) { protocolError("unexpected RQ_DATA message"); }

    void protocolError(const std::string& message) {
        ATTACK_ERR(message);
        endSession("protocol error");
    }

    void cancelPendingOverwrite() {
        std::lock_guard<std::mutex> lock(requestMutex_);
        awaitingResponse_ = false;
    }

    void requestReplacement(unsigned int turbineId, SignalType signalType) {
        {
            std::lock_guard<std::mutex> lock(requestMutex_);
            const auto now = clock_.steadyNow();
            if (awaitingResponse_ && now < requestExpiresAt_) return;
            awaitingResponse_ = true;
            requestedSignalType_ = signalType;
            requestedTurbineId_ = static_cast<int>(turbineId);
            requestExpiresAt_ = now + timing_.requestRetryPeriod;
        }

        const TimeStamp requestTime = clock_.unixTimeMilliseconds();
        const auto request = sc::protocol::attack::encode(RqDataMessage{
            static_cast<uint8_t>(turbineId),
            signalType,
            requestTime,
            requestTime + static_cast<TimeStamp>(timing_.requestLifetime.count())});
        if (!channel_.send(request.data(), request.size())) {
            cancelPendingOverwrite();
        }
    }

    void endSession(const std::string& reason) {
        const auto closed = sessionManager_.endSession(reason);
        if (!closed) return;
        cancelPendingOverwrite();
        attackStarted_.store(false);
        publishResourceUsage();
        audit(*closed, "disconnected");
    }

    void audit(const sc::application::AttackSessionInfo& session, const std::string& event) const {
        emitAudit(session.id, session.label, event);
    }

    void audit(const sc::application::ClosedAttackSession& session, const std::string& event) const {
        emitAudit(session.id, session.label, event + ";reason=" + session.reason);
    }

    void emitAudit(sc::application::AttackSessionId id,
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

    int numTurbines_;
    sc::ports::AttackChannel& channel_;
    sc::ports::Clock& clock_;
    AttackTiming timing_;
    sc::application::AttackSessionManager sessionManager_;

    mutable std::mutex auditMutex_;
    AuditCallback auditCallback_;
    std::atomic<bool> attackStarted_{false};

    std::mutex requestMutex_;
    bool awaitingResponse_{false};
    SignalType requestedSignalType_{SignalType::NONE};
    int requestedTurbineId_{0};
    sc::ports::Clock::SteadyTimePoint requestExpiresAt_{};
};

} // namespace AttackInterface
