#include "communication/AttackInterface.hpp"

#include "common/util.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>

namespace AttackInterface {
    const std::map<std::string, TxDataType> iecAttackInterfaceMap {
        {IEC_STRINGS::WS_MEAS, TX_WS},
        {IEC_STRINGS::WD_MEAS, TX_WD},
        {IEC_STRINGS::WTUR_TurSt, TX_ST},
        {IEC_STRINGS::POWER_MEAS, TX_PW},
        {IEC_STRINGS::YAW_MEAS, TX_YAW},
        {IEC_STRINGS::RPM_MEAS, TX_RPM},
        {IEC_STRINGS::PITCH_VAL, TX_PTCH},
    };

    Controller::Controller(int numTurbines, SocketWrapper& socketRef)
        : numTurbines_(numTurbines),
          socket_(socketRef) {
        for (int i = 0; i < numTurbines_; ++i) {
            LinkState linkState;
            linkState.tapEnabled = std::map<TxDataType, bool> {
                {TX_WS, false}, {TX_WD, false}, {TX_ST, false}, {TX_PW, false},
                {TX_YAW, false}, {TX_RPM, false}, {TX_PTCH, false}, {TX_SPT_YAW, false}, {TX_SPT_PWR, false},
            };
            linkState.fdiEnabled = std::map<TxDataType, bool> {
                {TX_WS, false}, {TX_WD, false}, {TX_ST, false}, {TX_PW, false},
                {TX_YAW, false}, {TX_RPM, false}, {TX_PTCH, false}, {TX_SPT_YAW, false}, {TX_SPT_PWR, false},
            };
            state_.linkStates.push_back(linkState);
        }

        socket_.attachAttackInterfaceCallback([this](const uint8_t* data, size_t length) {
            this->attackHandler(data, length);
        });
    }

    void Controller::parseControlCommand(const uint8_t* data, size_t length) {
        const size_t fullStructLength = sizeof(CtDataMessage) + static_cast<size_t>(numTurbines_);
        constexpr size_t compactPrefixLength = 4 + sizeof(ControlSignal) + sizeof(TxDataType);
        const size_t compactLength = compactPrefixLength + static_cast<size_t>(numTurbines_);

        const ControlSignal signal = *reinterpret_cast<const ControlSignal*>(data + 4);
        const TxDataType dataType = *reinterpret_cast<const TxDataType*>(data + 4 + sizeof(ControlSignal));

        const uint8_t* enableBytes = nullptr;
        if (length >= fullStructLength) {
            enableBytes = data + sizeof(CtDataMessage);
        } else if (length >= compactLength) {
            enableBytes = data + compactPrefixLength;
        } else {
            ATTACK_ERR("Invalid CT_DATA length: " << length << ", expected at least " << compactLength
                       << " (compact) or " << fullStructLength << " (full struct)");
            return;
        }

        ATTACK_LOG_V2("Parsed CT_DATA command with signal: " << static_cast<int>(signal)
                      << ", dataType: " << static_cast<int>(dataType));

        if (signal == CTRL_TAP) {
            for (int i = 0; i < numTurbines_; ++i) {
                const bool enabled = (enableBytes[static_cast<size_t>(i)] != 0);
                state_.linkStates[i].tapEnabled[dataType] = enabled;
                ATTACK_LOG_V1("Toggled control for turbine " << (i + 1)
                              << ", dataType " << static_cast<int>(dataType) << " to " << enabled);
            }
        } else if (signal == CTRL_FDI) {
            for (int i = 0; i < numTurbines_; ++i) {
                const bool enabled = (enableBytes[static_cast<size_t>(i)] != 0);
                state_.linkStates[i].fdiEnabled[dataType] = enabled;
                ATTACK_LOG_V1("Toggled false data injection for turbine " << (i + 1)
                              << ", dataType " << static_cast<int>(dataType) << " to " << enabled);
            }
        } else {
            ATTACK_ERR("Unsupported control signal received in CT_DATA: " << static_cast<int>(signal));
        }
    }

    void Controller::parseAttackDataCommand(const uint8_t* data, size_t length) {
        if (length < sizeof(AtDataMessage)) {
            ATTACK_ERR("Invalid AT_DATA length: " << length << ", expected at least " << sizeof(AtDataMessage));
            return;
        }

        {
            std::lock_guard<std::mutex> lock(state_.rqAtMutex);
            if (!state_.awaitingAtResponse || state_.atResponseReceived) {
                ATTACK_LOG_V2("Received unexpected AT_DATA message. Ignoring.");
                return;
            }
        }

        const AtDataMessage* msg = reinterpret_cast<const AtDataMessage*>(data);
        ATTACK_LOG_V2("Parsed AT_DATA command for turbine " << static_cast<int>(msg->turbineId)
                      << ", dataType " << static_cast<int>(msg->dataType) << ", fakeValue " << msg->spoofedValue);

        if (msg->dataType != state_.rqDataType || msg->turbineId != state_.rqTurbineId) {
            ATTACK_LOG_V2("Received AT_DATA does not match any pending RQ_DATA request. Ignoring.");
            return;
        }

        std::lock_guard<std::mutex> lock(state_.rqAtMutex);
        state_.atResponseReceived = state_.awaitingAtResponse;
        state_.awaitingAtResponse = false;
        state_.atResponseValue = msg->spoofedValue;
    }

    void Controller::parseConfigCommand(const uint8_t* data, size_t length) {
        if (length < sizeof(CfgDataMessage)) {
            ATTACK_ERR("Invalid CFG_DATA length: " << length << ", expected at least " << sizeof(CfgDataMessage));
            return;
        }

        const CfgDataMessage* msg = reinterpret_cast<const CfgDataMessage*>(data);

        CfgDataMessage parsed{};
        std::memcpy(parsed.teamName, msg->teamName, sizeof(parsed.teamName));
        parsed.teamName[sizeof(parsed.teamName) - 1] = '\0';
        parsed.scenarioId = msg->scenarioId;
        parsed.turbineController = msg->turbineController;

        resetState();

        ATTACK_LOG_V1("Parsed CFG_DATA command: teamName='" << parsed.teamName
                      << "', scenarioId=" << parsed.scenarioId << ", turbineController=" << parsed.turbineController);

        if (cfgCommandCallback_) {
            cfgCommandCallback_(parsed);
        }

        ATTACK_ST("Client connected by team " << parsed.teamName);
    }

    void Controller::parseSimulationControlCommand(const uint8_t* data, size_t length) {
        if (length < sizeof(SimCtrlMessage)) {
            ATTACK_ERR("Invalid SIM_CTRL length: " << length << ", expected at least " << sizeof(SimCtrlMessage));
            return;
        }

        const SimCtrlMessage* msg = reinterpret_cast<const SimCtrlMessage*>(data);
        SimCtrlMessage parsed{};
        parsed.simulationStart = msg->simulationStart;

        ATTACK_LOG_V1("Parsed SIM_CTRL command: simulationStart=" << parsed.simulationStart);

        if (simCtrlCommandCallback_) {
            simCtrlCommandCallback_(parsed);
        }

        socket_.txAttackInterfaceData(std::make_shared<SimCtrlMessage>(parsed), sizeof(SimCtrlMessage));
        ATTACK_ST("Started.");
    }

    void Controller::attackHandler(const uint8_t* data, size_t length) {
        if (length == 0) {
            return;
        }

        MessageType msgType = static_cast<MessageType>(data[0]);
        ATTACK_LOG_V1("Received message with header: " << static_cast<int>(msgType));

        switch (msgType) {
            case CT_DATA:
                parseControlCommand(data, length);
                break;
            case AT_DATA:
                parseAttackDataCommand(data, length);
                break;
            case CFG_DATA:
                parseConfigCommand(data, length);
                break;
            case SIM_CTRL:
                parseSimulationControlCommand(data, length);
                break;
            default:
                ATTACK_ERR("Unknown message type received: " << static_cast<int>(msgType));
        }
    }

    void Controller::resetState() {
        for (auto& linkState : state_.linkStates) {
            for (auto& [dataType, _] : linkState.tapEnabled) {
                linkState.tapEnabled[dataType] = false;
            }
            for (auto& [dataType, _] : linkState.fdiEnabled) {
                linkState.fdiEnabled[dataType] = false;
            }
        }
        txFails_ = 0;
    }

    void Controller::signalReady() {
        SimCtrlMessage msg;
        msg.simulationStart = true;
        socket_.txAttackInterfaceData(std::make_shared<SimCtrlMessage>(msg), sizeof(SimCtrlMessage));
        ATTACK_LOG_V1("Signaled readiness to start simulation to the attack interface client.");
    }

    void Controller::txData(unsigned int turbineId, TxDataType dataType, void* value) {
        if (dataType == TX_NONE) {
            return;
        }
        if (turbineId < 1 || turbineId > state_.linkStates.size()) {
            ATTACK_ERR("Invalid turbine ID: " << turbineId);
            return;
        }

        if (!state_.linkStates[turbineId - 1].tapEnabled[dataType]) {
            return;
        }

        ATTACK_LOG_V2("txData called for turbine " << turbineId << ", dataType " << static_cast<int>(dataType)
                      << ", value " << value);

        TxDataMessage msg;
        msg.turbineId = turbineId;
        msg.dataType = dataType;
        msg.value = *static_cast<float*>(value);
        socket_.txAttackInterfaceData(std::make_shared<TxDataMessage>(msg), sizeof(TxDataMessage));
    }

    ResultCode Controller::overwrite(unsigned int turbineId, TxDataType dataType, float& val) {
        if (dataType == TX_NONE) {
            return AI_DISABLED;
        }

        if (turbineId < 1 || turbineId > state_.linkStates.size()) {
            ATTACK_ERR("Invalid turbine ID: " << turbineId);
            return AI_ERROR;
        }

        if (!state_.linkStates[turbineId - 1].fdiEnabled[dataType]) {
            return AI_DISABLED;
        }

        ATTACK_LOG_V1("overwrite called for turbine " << turbineId << ", dataType " << static_cast<int>(dataType)
                      << ", original value " << val);

        txData(turbineId, dataType, &val);

        RqDataMessage rqMsg;
        rqMsg.turbineId = turbineId;
        rqMsg.dataType = dataType;
        rqMsg.requestTimeMs = static_cast<TimestampMs>(getCurrentTimeMs());
        rqMsg.expirationTimeMs = rqMsg.requestTimeMs + 250;

        {
            std::lock_guard<std::mutex> lock(state_.rqAtMutex);
            if (state_.awaitingAtResponse) {
                ATTACK_ERR("Already awaiting AT_DATA response for previous RQ_DATA. Cannot send new RQ_DATA yet.");
                return AI_ERROR;
            }
            state_.awaitingAtResponse = true;
            state_.atResponseReceived = false;
            state_.rqDataType = dataType;
            state_.rqTurbineId = turbineId;
        }

        socket_.txAttackInterfaceData(std::make_shared<RqDataMessage>(rqMsg), sizeof(RqDataMessage));

        const auto startTime = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() - startTime < timeout) {
            {
                std::lock_guard<std::mutex> lock(state_.rqAtMutex);
                if (state_.atResponseReceived) {
                    if (!std::isnan(state_.atResponseValue)) {
                        val = state_.atResponseValue;
                    }
                    state_.awaitingAtResponse = false;
                    state_.atResponseReceived = false;
                    ATTACK_LOG_V1("Received overwrite " << val);
                    txFails_ = 0;
                    return AI_OK;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        ATTACK_LOG_V1("Overwrite request timed out after " << timeout.count()
                      << " ms without receiving a response. (startime = " << startTime.time_since_epoch().count()
                      << ", now = " << std::chrono::steady_clock::now().time_since_epoch().count() << ")");
        {
            std::lock_guard<std::mutex> lock(state_.rqAtMutex);
            state_.awaitingAtResponse = false;
            state_.atResponseReceived = false;
            txFails_ += 1;
            if (txFails_ >= maxFails_) {
                ATTACK_ST("Attack Interface Disconnected");
                resetState();
            }
        }

        return AI_TIMEOUT;
    }

    void Controller::setCfgCommandCallback(CfgCommandCallback callback) {
        cfgCommandCallback_ = std::move(callback);
    }

    void Controller::setSimCtrlCommandCallback(SimCtrlCommandCallback callback) {
        simCtrlCommandCallback_ = std::move(callback);
    }
}
