#pragma once

#include "common/SharedData.hpp"
#include "common/config.hpp"
#include "sc/application/AttackSessionManager.hpp"
#include "sc/application/AttackSignalType.hpp"
#include "sc/ports/AttackChannel.hpp"
#include "sc/ports/Clock.hpp"

#include <chrono>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <sstream>
#include <iomanip>
#include <string>
#include <utility>
#include <vector>

namespace AttackInterface {

typedef enum eDataHeader {
    TX_DATA = 0x01,
    RQ_DATA = 0x02,
    AT_DATA = 0x04,
    CT_DATA = 0x08,
    CFG_DATA = 0x10,
    SIM_CTRL = 0x20,
    HEARTBEAT = 0x40,
    RELEASE = 0x80,
} DataHeader;

typedef DataHeader MessageType;

typedef enum eControlSignal {
    CTRL_NONE = 0x00,
    CTRL_TAP = 0x01,
    CTRL_FDI = 0x02,
} ControlSignal;

typedef uint64_t TimeStamp;

struct AttackTiming {
    std::chrono::milliseconds requestLifetime{250};
    std::chrono::milliseconds requestRetryPeriod{500};
    std::chrono::milliseconds sessionLeaseTimeout{750};
};

typedef struct sTxDataMessage {
    const uint8_t header = TX_DATA;
    uint8_t turbineId;
    SignalType dataType;
    const uint8_t payload_length = 0x01;
    float value;
} TxDataMessage;

typedef struct sRqDataMessage {
    const uint8_t header = RQ_DATA;
    uint8_t turbineId;
    SignalType dataType;
    TimeStamp rq_time;
    TimeStamp exp_time;
} RqDataMessage;

typedef struct sAtDataMessage {
    const uint8_t header = AT_DATA;
    uint8_t turbineId;
    SignalType dataType;
    TimeStamp at_time;
    float fake_value;
} AtDataMessage;

typedef struct sCtDataMessage {
    const uint8_t header = CT_DATA;
    ControlSignal signal;
    SignalType dataType;
    uint8_t* enable;
} CtDataMessage;

// Current configuration layout. Only teamName is used as the session label.
typedef struct sCfgDataMessage {
    const uint8_t header = CFG_DATA;
    char teamName[256];
    int scenarioId;
    int turbineController;
} CfgDataMessage;

// Retained for wire compatibility; simulation control is no longer accepted here.
typedef struct sSimCtrlMessage {
    const uint8_t header = SIM_CTRL;
    bool simStart;
} SimCtrlMessage;

typedef struct sHeartbeatMessage {
    const uint8_t header = HEARTBEAT;
} HeartbeatMessage;

typedef struct sReleaseMessage {
    const uint8_t header = RELEASE;
} ReleaseMessage;

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

        TxDataMessage message;
        message.turbineId = static_cast<uint8_t>(turbineId);
        message.dataType = signalType;
        message.value = *static_cast<float*>(value);
        channel_.send(reinterpret_cast<const uint8_t*>(&message), sizeof(message));
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

    void parseControl(const uint8_t* data, size_t length) {
        const size_t fullLength = sizeof(CtDataMessage) + static_cast<size_t>(numTurbines_);
        constexpr size_t compactPrefixLength = 4 + sizeof(ControlSignal) + sizeof(SignalType);
        const size_t compactLength = compactPrefixLength + static_cast<size_t>(numTurbines_);
        const uint8_t* enabled = nullptr;
        if (length >= fullLength) enabled = data + sizeof(CtDataMessage);
        else if (length >= compactLength) enabled = data + compactPrefixLength;
        else {
            protocolError("invalid CT_DATA length");
            return;
        }

        const auto session = sessionManager_.session();
        if (!session) {
            ATTACK_ERR("Ignoring CT_DATA without an active configured session");
            return;
        }

        ControlSignal control = CTRL_NONE;
        SignalType signalType = SignalType::NONE;
        std::memcpy(&control, data + 4, sizeof(control));
        std::memcpy(&signalType, data + 4 + sizeof(control), sizeof(signalType));
        if (control != CTRL_TAP && control != CTRL_FDI) {
            protocolError("unsupported control signal");
            return;
        }

        bool anyEnabled = false;
        for (int turbineId = 1; turbineId <= numTurbines_; ++turbineId) {
            const bool value = enabled[static_cast<size_t>(turbineId - 1)] != 0;
            const bool updated = control == CTRL_TAP
                ? sessionManager_.setTapEnabled(turbineId, signalType, value)
                : sessionManager_.setFdiEnabled(turbineId, signalType, value);
            if (!updated) {
                protocolError("unsupported attack signal type");
                return;
            }
            anyEnabled = anyEnabled || value;
            audit(*session,
                  std::string(control == CTRL_TAP ? "tap" : "fdi") +
                      ";signal=" + signalTypeName(signalType) +
                      ";turbine=" + std::to_string(turbineId) +
                      ";enabled=" + (value ? "true" : "false"));
        }
        publishResourceUsage();
        if (anyEnabled && !attackStarted_.exchange(true)) {
            audit(*session, "attack_started");
        }
    }

    void parseAttackData(const uint8_t* data, size_t length) {
        if (length < sizeof(AtDataMessage)) {
            protocolError("invalid AT_DATA length");
            return;
        }
        uint8_t turbineId = 0;
        SignalType signalType = SignalType::NONE;
        float fakeValue = 0.0F;
        std::memcpy(&turbineId, data + offsetof(AtDataMessage, turbineId), sizeof(turbineId));
        std::memcpy(&signalType, data + offsetof(AtDataMessage, dataType), sizeof(signalType));
        std::memcpy(&fakeValue, data + offsetof(AtDataMessage, fake_value), sizeof(fakeValue));

        if (std::isnan(fakeValue) ||
            !sessionManager_.setFdiValue(static_cast<int>(turbineId), signalType, fakeValue)) {
            return;
        }
        std::lock_guard<std::mutex> lock(requestMutex_);
        if (signalType == requestedSignalType_ && turbineId == requestedTurbineId_) {
            awaitingResponse_ = false;
        }
    }

    void parseConfiguration(const uint8_t* data, size_t length) {
        if (length < sizeof(CfgDataMessage)) {
            protocolError("invalid CFG_DATA length");
            return;
        }
        CfgDataMessage configuration{};
        std::memcpy(configuration.teamName,
                    data + offsetof(CfgDataMessage, teamName),
                    sizeof(configuration.teamName));
        configuration.teamName[sizeof(configuration.teamName) - 1] = '\0';

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
    }

    void handleMessage(const uint8_t* data, size_t length) {
        if (data == nullptr || length == 0) return;
        switch (static_cast<MessageType>(data[0])) {
            case CT_DATA: parseControl(data, length); break;
            case AT_DATA: parseAttackData(data, length); break;
            case CFG_DATA: parseConfiguration(data, length); break;
            case HEARTBEAT:
                if (length == sizeof(HeartbeatMessage)) sessionManager_.heartbeat();
                else protocolError("invalid HEARTBEAT length");
                break;
            case RELEASE:
                if (length == sizeof(ReleaseMessage)) endSession("client release");
                else protocolError("invalid RELEASE length");
                break;
            case SIM_CTRL:
                ATTACK_LOG_V1("Ignoring unused SIM_CTRL command");
                break;
            default:
                protocolError("unknown message type");
        }
    }

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

        RqDataMessage request;
        request.turbineId = static_cast<uint8_t>(turbineId);
        request.dataType = signalType;
        request.rq_time = clock_.unixTimeMilliseconds();
        request.exp_time = request.rq_time + static_cast<TimeStamp>(timing_.requestLifetime.count());
        if (!channel_.send(reinterpret_cast<const uint8_t*>(&request), sizeof(request))) {
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
