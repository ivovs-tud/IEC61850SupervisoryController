#pragma once

#include "common/PeriodicTask.hpp"
#include "common/GlobalDataStructure.hpp"
#include "common/DataHistorian.hpp"
#include "communication/libiec_wrapper.hpp"
#include "communication/socket/SocketWrapper.hpp"
#include "AttackInterface.hpp"

typedef enum rc
{
    COMM_OK = 0,
    COMM_ERROR = -1,
} CommReturnCode;

typedef enum cs
{
    COMM_DISCONNECTED = -1,
    COMM_CONNECTING = 0,
    COMM_CONNECTED = 1,
} CommStatus;

// ---------------------------------------------------------------------------
// CommConfig – single-argument configuration for the entire communication
// stack.  Each link has its own port and poll/update period so they can be
// tuned independently.  All fields carry ready-to-use defaults.
// ---------------------------------------------------------------------------
struct CommConfig
{
    // ZeroMQ operator server (HMI → controller)
    struct OperatorServer {
        int                       port        {9001};
        std::chrono::milliseconds pollPeriod  {std::chrono::milliseconds(10)};
    } operatorServer;

    // ZeroMQ attack-interface server (test harness → controller)
    struct AttackInterface {
        int                       port        {9002};
        std::chrono::milliseconds pollPeriod  {std::chrono::milliseconds(10)};
    } attackInterface;

    // TCP data historian server (external device/test source -> controller)
    struct DataHistorian {
        int                       port        {9003};
        std::chrono::milliseconds pollPeriod  {std::chrono::milliseconds(10)};
    } dataHistorian;

    // IEC 61850 MMS client
    struct Mms {
        std::vector<TurbineEndpoint>  turbines;   ///< one entry per turbine; IDs are 1-based
        std::chrono::milliseconds     pollPeriod  {std::chrono::milliseconds(100)};
    } mms;

    // IEC 61850 GOOSE subscriber (future)
    struct Goose {
        std::string               networkInterface{"veth1"};
        std::chrono::milliseconds pollPeriod  {std::chrono::milliseconds(4)};
    } goose;

    // CommunicationTask orchestration loop cadence
    std::chrono::milliseconds orchestrationPeriod{std::chrono::milliseconds(100)};
};

// ---------------------------------------------------------------------------
// CommunicationTask – orchestrates IEC 61850 and TCP socket communications.
// ---------------------------------------------------------------------------
class CommunicationTask : public PeriodicTask
{
public:
    explicit CommunicationTask(DataHistorian& dataHistorian, const CommConfig& config = CommConfig{});

    void init();



protected:
    void onStart() override;  // starts socket servers before entering the loop
    void execute() override;  // periodic comms polling
    void onStop()  override;  // stops socket servers after the loop exits

private:
    struct CommunicationState
    {
        std::atomic<CommStatus> iec_status;
        std::atomic<CommStatus> socket_status;
        std::chrono::system_clock::time_point lastActivityTime;
    } state;

    CommConfig    config_;
    libiec_wrapper iecWrapper_;
    SocketWrapper socketWrapper;
    AttackInterface::AttackInterface attackInterface;
    DataHistorian& dataHistorian_;

    // Runtime state for descriptor scheduling (next execution times in UNIX ms)
    // Key: "turbineId:descriptorName" (e.g., "1:power setpoint")
    mutable std::mutex rxDescriptorMutex_;
    mutable std::mutex txDescriptorMutex_;
    std::map<std::string, uint64_t> rxNextExecutionTimes_;  ///< maps descriptor key to next execution time (UNIX ms)
    std::map<std::string, uint64_t> txNextExecutionTimes_;  ///< maps descriptor key to next execution time (UNIX ms)

    // -----------------------------------------------------------------------
    // Descriptor-driven per-turbine operation helpers called by execute().
    //
    // RxDescriptor: one float measurement to read from a turbine, eavesdrop
    //   via AttackInterface, and store in GlobalDataStructure.
    // TxDescriptor: one float setpoint to read from GlobalDataStructure,
    //   eavesdrop/intercept via AttackInterface, and write to a turbine.
    //
    // To register a new measurement : append a row to RX_DESCRIPTORS in .cpp.
    // To register a new setpoint    : append a row to TX_DESCRIPTORS in .cpp.
    //
    // turbineId is 1-based (IEC 61850 convention).
    // idx       is 0-based (GlobalDataStructure array index).
    // -----------------------------------------------------------------------
    struct RxDescriptor {
        const char*                              name;
        const char*                              unit;
        IECReturnCode (libiec_wrapper::*iecRead)(int, float&);
        AttackInterface::TxDataType              txDataType;
        std::vector<double> GlobalData::*        lastField;
        TurbineHistory<double> GlobalData::*     historyField;
        uint32_t                                 intervalMs;  ///< interval between RX operations in milliseconds
    };

    struct TxDescriptor {
        const char*                                  name;
        const char*                                  unit;
        std::function<float(const GlobalData&, int)> gdsRead;
        AttackInterface::TxDataType                  txDataType;
        IECReturnCode (libiec_wrapper::*iecWrite)(int, float);
        uint32_t                                     intervalMs;  ///< interval between TX operations in milliseconds
    };

    static const RxDescriptor RX_DESCRIPTORS[];
    static const TxDescriptor TX_DESCRIPTORS[];

    // -----------------------------------------------------------------------
    // Timestamp control methods: allow prescribing RX/TX frequencies
    // -----------------------------------------------------------------------
    
    /// Get the next execution time (UNIX timestamp in ms) for an RX descriptor
    uint64_t getRxNextExecutionTimeMs(int turbineId, const RxDescriptor& desc) const;
    
    /// Get the next execution time (UNIX timestamp in ms) for a TX descriptor
    uint64_t getTxNextExecutionTimeMs(int turbineId, const TxDescriptor& desc) const;
    
    /// Overwrite the next execution time for an RX descriptor (in UNIX timestamp ms)
    void setRxNextExecutionTimeMs(int turbineId, const RxDescriptor& desc, uint64_t timeMs);
    
    /// Overwrite the next execution time for a TX descriptor (in UNIX timestamp ms)
    void setTxNextExecutionTimeMs(int turbineId, const TxDescriptor& desc, uint64_t timeMs);
    
    /// Get a descriptor key for internal map lookups
    std::string getDescriptorKey(int turbineId, const char* descriptorName) const;

    void doRxMeasurement(int turbineId, int idx, const RxDescriptor& desc);
    void doTxSetpoint   (int turbineId, int idx, const TxDescriptor& desc);
    void doRxSecret     (int turbineId);
};
