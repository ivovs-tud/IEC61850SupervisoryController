#pragma once

#include <iomanip>
#include <iostream>
#include <map>
#include <string>

#include "common/config.hpp"
#include "libiec_wrapper.hpp"
#include "socket/SocketWrapper.hpp"

namespace AttackInterface
{
    typedef enum eDataHeader
    {
        TX_DATA = 0x01,     // Data just for transmission
        RQ_DATA = 0x02,     // Request for sending specific data
        AT_DATA = 0x04,     // Data containing overwrite signals (response to RQ_DATA)
        CT_DATA = 0x08,     // Control data (e.g. containing)
    } DataHeader;

    typedef DataHeader MessageType;

    typedef enum eTxDataType {
        TX_WS = 0x01,       // Wind Speed Data
        TX_WD = 0x02,       // Wind Direction Data
        TX_ST = 0x03,       // Turbine Status Data
        TX_PW = 0x04,       // Power generation Data
        TX_YAW = 0x05,      // Yaw angle Data
        TX_RPM = 0x06,      // Rotor speed Data
        TX_PTCH = 0x07,     // Pitch angle Data        
        TX_SPT_YAW = 0x08,   // Yaw setpoint Data
        TX_SPT_PWR = 0x09,   // Power setpoint Data
        TX_ARRAY = 0xFF,    // Here for extensibility, not currently used
    } TxDataType;

    const std::map<std::string, TxDataType> iecAImap {
        {IEC_STRINGS::WS_MEAS, TX_WS},
        {IEC_STRINGS::WD_MEAS, TX_WD},
        {IEC_STRINGS::WTUR_TurSt, TX_ST},
        {IEC_STRINGS::POWER_MEAS, TX_PW},
        {IEC_STRINGS::YAW_MEAS, TX_YAW},
        {IEC_STRINGS::RPM_MEAS, TX_RPM},
        {IEC_STRINGS::PITCH_VAL, TX_PTCH},
    };


    typedef enum eControlSignal
    {
        CTRL_NONE = 0x00,           // Nothing
        CTRL_TAP = 0x01,            // Start/stop tapping communication
        CTRL_FDI = 0x02,            // Start/stop false data injection attack
    } ControlSignal;

    
    typedef uint64_t TimeStamp; // Unix timestamp in milliseconds

    // Message Structure Definitions
    typedef struct sTxDataMessage {
        const uint8_t header = TX_DATA;
        uint8_t turbineId;      // 1-based turbine ID
        TxDataType dataType;    // Type of data being sent
        const uint8_t payload_length = 0x01;
        float value;            // Value of the data
    } TxDataMessage;

    typedef struct sRqDataMessage {
        const uint8_t header = RQ_DATA;
        uint8_t turbineId;      // 1-based turbine ID
        TxDataType dataType;    // Type of data being requested
        TimeStamp rq_time;      // Timestamp of the request
        TimeStamp exp_time;     // Timestamp of when the request will be expired
    } RqDataMessage;

    typedef struct sAtDataMessage {
        const uint8_t header = AT_DATA;
        uint8_t turbineId;      // 1-based turbine ID
        TxDataType dataType;    // Type of data being overwritten
        TimeStamp at_time;      // Timestamp of when the attack should be executed
        float fake_value;       // The false value to inject
    } AtDataMessage;

    typedef struct sCtDataMessage {
        const uint8_t header = CT_DATA;
        ControlSignal signal;       // Control type
        TxDataType dataType;        // Type of data the control signal is related to (if applicable)
        uint8_t *enable;            // Enable/disable control of this datatype for each turbine
    } CtDataMessage;   

    typedef enum eAIRC {
        AI_OK = 0,
        AI_ERROR = -1,
        AI_TIMEOUT = -2
    } AIRC;

    class AttackInterface {
        /**
         * @brief This class will serve as a binder that :
         *  1. Translates data structs to raw payloads and vice versa
         *  2. Contains the logic for keeping track of controlled data types for each turbine
         *  3. Handles hooks for attachment points in the communication flow (e.g. when a new setpoint is sent or a new measurement is received)
         * 
         */
        
        private:
            int numTurbines; // Number of turbines in the system, used for bounds checking and vector sizing

            typedef struct sLinkState {
                /**
                 * @brief Struct to keep track of the state of signals on one communication link (between the SC and one turbine)
                 * 
                 */
                std::map<TxDataType, bool> tapEnabled; // Whether attack interface control is enabled for each data type
                std::map<TxDataType, bool> fdiEnabled; // Whether false data injection is enabled for each data type
            } LinkState;

            struct  {
                std::vector<LinkState> LinkStates; // 1-based index for turbines

                // Variables needed for proper handling or RQ_ and AT_ messages
                std::mutex rq_at_mutex_;        // mutex to protect the following variables
                bool awaiting_at_response = false; // whether we are currently waiting for an AT_DATA message in response to a RQ_DATA message
                bool at_response_received = false; // whether we have received the expected AT_DATA message in response to a RQ_DATA message
                float at_response_val = 0.0f; // the value received in the AT_DATA message in response to a RQ_DATA message
                TxDataType rq_DataType; // the data type of the last RQ_DATA message, used to validate incoming AT_DATA messages
                int rq_TurbineId; // the turbine ID of the last RQ_DATA message, used to validate incoming AT_DATA messages
            } state;

            

            SocketWrapper& socket;

            void parseCTCommand(const uint8_t* data, size_t length) {
                // Native-layout wire format used by the harness:
                // [CtDataMessage struct bytes][enable_1]...[enable_N]
                // We can reinterpret_cast for header/signal/dataType, but never dereference
                // msg->enable because that pointer value is sender-process local.
                const size_t fullStructLength = sizeof(CtDataMessage) + static_cast<size_t>(numTurbines);

                // Some senders serialize only up to dataType (with 4-byte alignment)
                // and then append enable bytes, which yields:
                // [header+pad(4)][signal(4)][dataType(4)][enable_1..enable_N]
                constexpr size_t compactPrefixLength = 4 + sizeof(ControlSignal) + sizeof(TxDataType);
                const size_t compactLength = compactPrefixLength + static_cast<size_t>(numTurbines);

                const ControlSignal signal = *reinterpret_cast<const ControlSignal *>(data + 4);
                const TxDataType dataType = *reinterpret_cast<const TxDataType *>(data + 4 + sizeof(ControlSignal));

                const uint8_t* enableBytes = nullptr;
                if (length >= fullStructLength) {
                    enableBytes = data + sizeof(CtDataMessage);
                } else if (length >= compactLength) {
                    enableBytes = data + compactPrefixLength;
                } else {
                    ATTACK_ERR("Invalid CT_DATA length: " << length
                               << ", expected at least " << compactLength
                               << " (compact) or " << fullStructLength << " (full struct)");
                    return;
                }

                ATTACK_LOG_V2("Parsed CT_DATA command with signal: " << static_cast<int>(signal)
                              << ", dataType: " << static_cast<int>(dataType));

                if (signal == CTRL_TAP) {
                    for (int i = 0; i < numTurbines; ++i) {
                        const bool enabled = (enableBytes[static_cast<size_t>(i)] != 0);
                        state.LinkStates[i].tapEnabled[dataType] = enabled;
                        ATTACK_LOG_V1("Toggled control for turbine " << (i + 1)
                                      << ", dataType " << static_cast<int>(dataType)
                                      << " to " << enabled);
                    }
                } else if(signal == CTRL_FDI) {
                    for (int i = 0; i < numTurbines; ++i) {
                        const bool enabled = (enableBytes[static_cast<size_t>(i)] != 0);
                        state.LinkStates[i].fdiEnabled[dataType] = enabled;
                        ATTACK_LOG_V1("Toggled false data injection for turbine " << (i + 1)
                                      << ", dataType " << static_cast<int>(dataType)
                                      << " to " << enabled);
                    }

                } else {
                    ATTACK_ERR("Unsupported control signal received in CT_DATA: "
                               << static_cast<int>(signal));
                }
            }

            void parseATCommand(const uint8_t* data, size_t length) {
                if (length < sizeof(AtDataMessage)) {
                    ATTACK_ERR("Invalid AT_DATA length: " << length
                               << ", expected at least " << sizeof(AtDataMessage));
                    return;
                }

                const AtDataMessage* msg = reinterpret_cast<const AtDataMessage*>(data);
                ATTACK_LOG_V1("Parsed AT_DATA command for turbine " << static_cast<int>(msg->turbineId)
                              << ", dataType " << static_cast<int>(msg->dataType)
                              << ", fakeValue " << msg->fake_value);

                // Check if this is a response to a RQ_DATA message we sent
                if (msg->dataType != state.rq_DataType || msg->turbineId != state.rq_TurbineId) {
                    ATTACK_ERR("Received AT_DATA does not match any pending RQ_DATA request. Ignoring.");
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(state.rq_at_mutex_);
                    state.awaiting_at_response = false;
                    state.at_response_received = true;
                    state.at_response_val = msg->fake_value;
                }
            }

            void AttackHandler(const uint8_t* data, size_t length) {
                if (length == 0) return;

                MessageType msgType = static_cast<MessageType>(data[0]);
                ATTACK_LOG_V1("Received message with header: " << static_cast<int>(msgType));

                switch(msgType) {
                    case CT_DATA:
                        parseCTCommand(data, length);
                        break;
                    case AT_DATA:
                        parseATCommand(data, length);
                        break;
                    default:
                        ATTACK_ERR("Unknown message type received: " << static_cast<int>(msgType));
                }
            }

        public:
            AttackInterface(int numTurbines, SocketWrapper& socketRef) : 
            numTurbines(numTurbines), socket(socketRef) {
                for (int i = 0; i < numTurbines; ++i) {
                    LinkState ls;
                    ls.tapEnabled = std::map<TxDataType, bool> {
                        {TX_WS, false}, {TX_WD, false}, {TX_ST, false}, {TX_PW, false}, 
                        {TX_YAW, false}, {TX_RPM, false}, {TX_PTCH, false}, {TX_SPT_YAW, false}, {TX_SPT_PWR, false},
                    };
                    state.LinkStates.push_back(ls);
                }

                socket.AttachAttackInterfaceCallback([this](const uint8_t* data, size_t length) {
                    this->AttackHandler(data, length);
                });
            }

            void txData(int turbineId, TxDataType dataType, float value) {
                if (turbineId < 1 || turbineId > state.LinkStates.size()) {
                    ATTACK_ERR("Invalid turbine ID: " << turbineId);
                    return;
                }

                if (!state.LinkStates[turbineId - 1].tapEnabled[dataType]) return;

                ATTACK_LOG_V2("txData called for turbine " << turbineId << ", dataType " << static_cast<int>(dataType)
                              << ", value " << value);

                // First, we create a TxDataMessage, and pass a shared_ptr to the socket wrapper's tx function
                TxDataMessage msg;
                msg.turbineId = turbineId;
                msg.dataType = dataType;
                msg.value = value;
                socket.txAttackInterfaceData(std::make_shared<TxDataMessage>(msg), sizeof(TxDataMessage));
            }


            AIRC overwrite(int turbineId, TxDataType dataType, float &val) {
                /**
                 * @brief If the control for the given dataType is enabled, then the value is overwritten by the attack interface
                 * 
                 * How this works: If enabled, a RQ_Message is send to the client to provide a new value to overwrite with. 
                 * The attack interface then waits for a response from the client with the new value, and returns it. If no response is received within a timeout, or if an error occurs, an error code is returned.
                 */

                 if (turbineId < 1 || turbineId > state.LinkStates.size()) {
                          ATTACK_ERR("Invalid turbine ID: " << turbineId);
                    return AI_ERROR;
                }

                if (!state.LinkStates[turbineId - 1].fdiEnabled[dataType]) return AI_OK;

                ATTACK_LOG_V1("overwrite called for turbine " << turbineId << ", dataType " << static_cast<int>(dataType)
                              << ", original value " << val);

                // 1. Send RQ_DATA message to client
                RqDataMessage rqMsg;
                rqMsg.turbineId = turbineId;
                rqMsg.dataType = dataType;
                rqMsg.rq_time = static_cast<TimeStamp>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
                rqMsg.exp_time = rqMsg.rq_time + 100; // Request expires after 100 milliseconds. TODO: Make this configurable

                {
                    std::lock_guard<std::mutex> lock(state.rq_at_mutex_);
                    if (state.awaiting_at_response) {
                        ATTACK_ERR("Already awaiting AT_DATA response for previous RQ_DATA. Cannot send new RQ_DATA yet.");
                        return AI_ERROR;
                    }
                    state.awaiting_at_response = true;
                    state.at_response_received = false;
                    state.rq_DataType = dataType;
                    state.rq_TurbineId = turbineId;
                }

                socket.txAttackInterfaceData(std::make_shared<RqDataMessage>(rqMsg), sizeof(RqDataMessage));

                // 2. Wait for AT_DATA response with a timeout
                const auto startTime = std::chrono::steady_clock::now();
                const auto timeout = std::chrono::milliseconds(100);
                while(std::chrono::steady_clock::now() - startTime < timeout) {
                    {
                        std::lock_guard<std::mutex> lock(state.rq_at_mutex_);
                        if (state.at_response_received) {
                            val = state.at_response_val;
                            state.awaiting_at_response = false;
                            state.at_response_received = false;
                            ATTACK_LOG_V1("Received overwrite " << val);
                            return AI_OK;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                // If we timed out, return an error
                {
                    std::lock_guard<std::mutex> lock(state.rq_at_mutex_);
                    state.awaiting_at_response = false;
                    state.at_response_received = false;
                }

                ATTACK_LOG_V1("Overwrite request timed out");
                return AI_TIMEOUT;
            }
            
    };
}


