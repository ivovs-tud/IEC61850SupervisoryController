#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "common/config.hpp"
#include "libiec_wrapper.hpp"
#include "socket/SocketWrapper.hpp"

namespace AttackInterface {
    /**
     * First byte of every attack-interface packet.
     *
     * These values are part of the external test-harness protocol. Rename the
     * C++ symbols if needed, but keep numeric values synchronized with clients.
     */
    typedef enum eDataHeader {
        TX_DATA = 0x01,      // controller -> client: tapped measurement/setpoint
        RQ_DATA = 0x02,      // controller -> client: request spoofed value
        AT_DATA = 0x04,      // client -> controller: spoofed value response
        CT_DATA = 0x08,      // client -> controller: enable tap/FDI per turbine
        CFG_DATA = 0x10,     // client -> controller: scenario and team metadata
        SIM_CTRL = 0x20,     // client -> controller: simulation start/ready signal
    } DataHeader;

    typedef DataHeader MessageType;

    /**
     * Signal identifiers shared with the attack-interface client.
     *
     * `TX_NONE` is deliberately out-of-band: it marks local control messages
     * that should be written to the turbine but not exposed for tap/FDI.
     */
    typedef enum eTxDataType {
        TX_WS = 0x01,
        TX_WD = 0x02,
        TX_ST = 0x03,
        TX_PW = 0x04,
        TX_YAW = 0x05,
        TX_RPM = 0x06,
        TX_PTCH = 0x07,
        TX_SPT_YAW = 0x08,
        TX_SPT_PWR = 0x09,
        TX_GENTORQ = 0x10,
        TX_OP_CMD = 0x0A,
        TX_NONE = 0xFE,
        TX_ARRAY = 0xFF,
    } TxDataType;

    extern const std::map<std::string, TxDataType> iecAttackInterfaceMap;

    typedef enum eControlSignal {
        CTRL_NONE = 0x00,
        CTRL_TAP = 0x01,     // stream matching values to the client
        CTRL_FDI = 0x02,     // allow the client to overwrite matching values
    } ControlSignal;

    typedef uint64_t TimestampMs; // Unix timestamp in milliseconds

    /**
     * Wire message definitions.
     *
     * These structs are copied directly to/from socket payloads. Keep default
     * member initializers for packet headers so callers cannot accidentally
     * send the wrong message type.
     */
    typedef struct sTxDataMessage {
        const uint8_t header = TX_DATA;
        uint8_t turbineId;              // 1-based turbine ID
        TxDataType dataType;
        const uint8_t payloadLength = 0x01;
        float value;
    } TxDataMessage;

    typedef struct sRqDataMessage {
        const uint8_t header = RQ_DATA;
        uint8_t turbineId;              // 1-based turbine ID
        TxDataType dataType;
        TimestampMs requestTimeMs;
        TimestampMs expirationTimeMs;
    } RqDataMessage;

    typedef struct sAtDataMessage {
        const uint8_t header = AT_DATA;
        uint8_t turbineId;              // 1-based turbine ID
        TxDataType dataType;
        TimestampMs attackTimeMs;
        float spoofedValue;
    } AtDataMessage;

    typedef struct sCtDataMessage {
        const uint8_t header = CT_DATA;
        ControlSignal signal;
        TxDataType dataType;
        uint8_t *enable;                // variable-length per-turbine enable bytes follow the fixed header
    } CtDataMessage; 

    typedef struct sCfgDataMessage {
        const uint8_t header = CFG_DATA;
        char teamName[256];
        int scenarioId;
        int turbineController;
    } CfgDataMessage;

    typedef struct sSimCtrlMessage {
        const uint8_t header = SIM_CTRL;
        bool simulationStart;           // sent by client to start; echoed by controller as ready/ack
    } SimCtrlMessage;

    using CfgCommandCallback = std::function<void(const CfgDataMessage&)>;
    using SimCtrlCommandCallback = std::function<void(const SimCtrlMessage&)>;

    enum ResultCode {
        AI_OK = 1,
        AI_DISABLED = 0,
        AI_ERROR = -1,
        AI_TIMEOUT = -2
    };

    /**
     * Protocol adapter for the attack/test interface.
     *
     * The controller keeps two independent enable maps per turbine:
     * - tap: publish matching values to the client without changing them;
     * - FDI: request a spoofed replacement before forwarding the value.
     *
     * `overwrite()` is synchronous because the IEC communication path needs a
     * concrete value before continuing. Timeouts therefore reset only the
     * pending request state, not the full socket server.
     */
    class Controller {
    private:
        int numTurbines_;

        typedef struct sLinkState {
            std::map<TxDataType, bool> tapEnabled;
            std::map<TxDataType, bool> fdiEnabled;
        } LinkState;

        struct State {
            std::vector<LinkState> linkStates;
            std::mutex rqAtMutex;
            bool awaitingAtResponse {false};
            bool atResponseReceived {false};
            float atResponseValue {0.0f};
            TxDataType rqDataType {TX_NONE};
            int rqTurbineId {0};
        } state_;

        SocketWrapper& socket_;
        CfgCommandCallback cfgCommandCallback_;
        SimCtrlCommandCallback simCtrlCommandCallback_;
        int txFails_ {0};
        const int maxFails_ {10};

        void parseControlCommand(const uint8_t* data, size_t length);
        void parseAttackDataCommand(const uint8_t* data, size_t length);
        void parseConfigCommand(const uint8_t* data, size_t length);
        void parseSimulationControlCommand(const uint8_t* data, size_t length);
        void attackHandler(const uint8_t* data, size_t length);

    public:
        Controller(int numTurbines, SocketWrapper& socketRef);

        void resetState();
        void signalReady();
        void txData(unsigned int turbineId, TxDataType dataType, void* value);
        ResultCode overwrite(unsigned int turbineId, TxDataType dataType, float& val);
        void setCfgCommandCallback(CfgCommandCallback callback);
        void setSimCtrlCommandCallback(SimCtrlCommandCallback callback);
    };
}
