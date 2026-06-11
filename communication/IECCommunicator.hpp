#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <chrono>

#include "common/PeriodicTask.hpp"
#include "common/GlobalDataStructure.hpp"
#include "common/DataHistorian.hpp"
#include "communication/libiec_wrapper.hpp"
#include "communication/AttackInterface.hpp"
#include "communication/CommunicationTypes.hpp"
#include "common/config.hpp"
#include "common/util.hpp"
#include "common/ConsoleColors.hpp"

/**
 * Per-turbine IEC bridge.
 *
 * One communicator owns two periodic workers: RX consumes measurements or
 * buffered reports, while TX publishes the latest control setpoints. Both
 * workers share the same turbine ID and IEC wrapper, so this class is also the
 * synchronization point for report buffering and activity timestamps.
 */
class IECCommunicator {
public:
    explicit IECCommunicator(const CommConfig& config,
                             int turbineId,
                             LibIecWrapper& iecWrapper,
                             AttackInterface::Controller& attackInterface,
                             std::mutex& attackInterfaceMutex);
    ~IECCommunicator();

    void start();
    void stop();

    int turbineId() const { return turbineId_; }
    CommStatus status() const { return iecStatus_.load(); }
    std::chrono::system_clock::time_point lastActivityTime() const;

private:
    class RxTask : public PeriodicTask {
    public:
        RxTask(IECCommunicator& owner, std::chrono::milliseconds period)
            : PeriodicTask(period),
              owner_(owner) {}

    private:
        void execute() override { owner_.executeRx(); }

        IECCommunicator& owner_;
    };

    class TxTask : public PeriodicTask {
    public:
        TxTask(IECCommunicator& owner, std::chrono::milliseconds period)
            : PeriodicTask(period),
              owner_(owner) {}

    private:
        void execute() override { owner_.executeTx(); }

        IECCommunicator& owner_;
    };

    enum class IecValueType {
        Float32,
        Int32,
        UInt32,
        Bool,
        Enum,
    };

    /**
     * Measurement mapping used by both polling and IEC report processing.
     *
     * `daReference` is the normal MMS data attribute reference. `reportReference`
     * is the report dataset member suffix emitted by libiec61850. Both map to
     * the same `GlobalData` storage so polling/reporting can share the update
     * path.
     */
    struct RxDescriptor {
        const char*                              name;
        const char*                              unit;
        const char*                              daReference;
        const char*                              reportReference;
        IECReturnCode (LibIecWrapper::*readValue)(int, float&);
        AttackInterface::TxDataType              txDataType;
        std::vector<double> GlobalData::*        lastValueField;
        TurbineHistory<double> GlobalData::*     historyField;
        std::vector<uint64_t> GlobalData::*      timestampField;
        uint32_t                                 intervalMs;
    };

    /**
     * Setpoint/command mapping from GlobalData to IEC writes.
     *
     * `globalDataValue` returns a pointer into the locked GlobalData object.
     * Callers must not store that pointer beyond the immediate write path.
     */
    struct TxDescriptor {
        const char*                                  name;
        IecValueType                                 valueType;
        std::function<void*(GlobalData&, int)>       globalDataValue;
        AttackInterface::TxDataType                  txDataType;
        IECReturnCode (LibIecWrapper::*writeValue)(int, void*);
        uint32_t                                     intervalMs;
    };

    static std::string descToString(void* value, const TxDescriptor& desc);
    uint64_t getRxNextExecutionTimeMs(size_t index) const;
    uint64_t getTxNextExecutionTimeMs(size_t index) const;
    void setRxNextExecutionTimeMs(size_t index, uint64_t timeMs);
    void setTxNextExecutionTimeMs(size_t index, uint64_t timeMs);

    void executeRx();
    void executeTx();
    void touchActivityTime();
    void doTxSetpoint(size_t idx, const TxDescriptor& desc);
    void doRxMeasurement(size_t idx, const RxDescriptor& desc);
    void processRxMeasurement(const RxDescriptor& desc, float value, uint64_t timestampMs);
    void doRxSecret();
    bool reportingEnabled() const;
    void startReporting();
    void stopReporting();
    void handleReportValues(int turbineId, const std::vector<IecReportValue>& values);
    std::vector<std::string> reportFallbackReferences() const;
    std::optional<size_t> findRxDescriptorByReference(const std::string& reference) const;

    const CommConfig& config_;
    int turbineId_;
    LibIecWrapper& iecWrapper_;
    AttackInterface::Controller& attackInterface_;
    std::mutex& attackInterfaceMutex_;

    std::atomic<CommStatus> iecStatus_{COMM_DISCONNECTED};
    std::chrono::system_clock::time_point lastActivityTime_;
    mutable std::mutex lastActivityTimeMutex_;
    RxTask rxTask_;
    TxTask txTask_;

    std::vector<uint64_t> rxNextExecutionTimes_;
    std::vector<uint64_t> txNextExecutionTimes_;

    struct BufferedRxMeasurement {
        float value {0.0f};
        uint64_t timestampMs {0};
    };

    // Report callbacks arrive on the IEC manager path; RX swaps this buffer so
    // measurement processing still happens on the communicator's RX task.
    std::vector<std::optional<BufferedRxMeasurement>> reportRxBuffer_;
    std::mutex reportRxBufferMutex_;
    std::atomic<bool> reportStarted_ {false};

    static const RxDescriptor kRxDescriptors[];
    static const TxDescriptor kTxDescriptors[];
};
