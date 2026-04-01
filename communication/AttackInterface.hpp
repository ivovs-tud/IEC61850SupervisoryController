#pragma once

#include <iomanip>
#include <iostream>
#include <map>
#include <string>

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
                std::map<TxDataType, bool> controlEnabled; // Whether attack interface control is enabled for each data type
            } LinkState;

            struct  {
                std::vector<LinkState> LinkStates; // 1-based index for turbines


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

                const ControlSignal signal =
                  *reinterpret_cast<const ControlSignal *>(data + 4);
                const TxDataType dataType =
                  *reinterpret_cast<const TxDataType *>(data + 4 + sizeof(ControlSignal));

                const uint8_t* enableBytes = nullptr;
                if (length >= fullStructLength) {
                    enableBytes = data + sizeof(CtDataMessage);
                } else if (length >= compactLength) {
                    enableBytes = data + compactPrefixLength;
                } else {
                    std::cerr << "[AT] Invalid CT_DATA length: " << length
                              << ", expected at least " << compactLength
                              << " (compact) or " << fullStructLength << " (full struct)\n";
                    return;
                }

                std::cout << "[AT] Parsed CT_DATA command with signal: " << static_cast<int>(signal)
                          << ", dataType: " << static_cast<int>(dataType) << "\n";

                if (signal == CTRL_TAP) {
                    for (int i = 0; i < numTurbines; ++i) {
                        const bool enabled = (enableBytes[static_cast<size_t>(i)] != 0);
                        state.LinkStates[i].controlEnabled[dataType] = enabled;
                        std::cout << "[AT] Toggled control for turbine " << (i + 1)
                                  << ", dataType " << static_cast<int>(dataType)
                                  << " to " << enabled << "\n";
                    }
                } else {
                    std::cerr << "[AT] Unsupported control signal received in CT_DATA: "
                              << static_cast<int>(signal) << "\n";
                }
            }

            void AttackHandler(const uint8_t* data, size_t length) {
                if (length == 0) return;

                std::cout << "[AT] Raw bytes (hex, len=" << length << "): ";
                std::ios_base::fmtflags originalFlags = std::cout.flags();
                char originalFill = std::cout.fill();
                for (size_t i = 0; i < length; ++i) {
                    std::cout << std::hex << std::setw(2) << std::setfill('0')
                              << static_cast<unsigned int>(data[i]);
                    if (i + 1 < length) {
                        std::cout << ' ';
                    }
                }
                std::cout.flags(originalFlags);
                std::cout.fill(originalFill);
                std::cout << "\n";

                MessageType msgType = static_cast<MessageType>(data[0]);
                std::cout << "[AT] Received message with header: " << static_cast<int>(msgType) << std::endl;

                // if (length != sizeof(CtDataMessage)) {
                //     std::cerr << "[AT] Invalid message length: " << length << ". Expected: " << sizeof(CtDataMessage) << std::endl;
                //     return;
                // }

                switch(msgType) {
                    case CT_DATA:
                        parseCTCommand(data, length);
                        break;
                    default:
                        std::cerr << "[AT] Unknown message type received: " << static_cast<int>(msgType) << std::endl;
                }
            }

        public:
            AttackInterface(int numTurbines, SocketWrapper& socketRef) : 
            numTurbines(numTurbines), socket(socketRef) {
                for (int i = 0; i < numTurbines; ++i) {
                    LinkState ls;
                    ls.controlEnabled = std::map<TxDataType, bool> {
                        {TX_WS, true}, {TX_WD, true}, {TX_ST, false}, {TX_PW, false}, 
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
                    std::cerr << "[AT] Invalid turbine ID: " << turbineId << "\n";
                    return;
                }

                std::cout << "[AT] txData called for turbine " << turbineId << ", dataType " << static_cast<int>(dataType) 
                          << ", value " << value << "\n";

                if (state.LinkStates[turbineId - 1].controlEnabled[dataType]) {
                    // First, we create a TxDataMessage, and pass a shared_ptr to the socket wrapper's tx function
                    TxDataMessage msg;
                    msg.turbineId = turbineId;
                    msg.dataType = dataType;
                    msg.value = value;
                    socket.txAttackInterfaceData(std::make_shared<TxDataMessage>(msg), sizeof(TxDataMessage));
                }
            }

            MessageType parseMessage(const std::vector<uint8_t>& rawData) {
                if (rawData.empty()) {
                    throw std::runtime_error("Empty message received");
                }
                return static_cast<MessageType>(rawData[0]);
            }
            
    };
}


