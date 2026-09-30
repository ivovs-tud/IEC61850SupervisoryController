#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <chrono>

#include "sc/runtime/PeriodicTask.hpp"
#include "sc/model/SharedData.hpp"
#include "sc/runtime/DataHistorian.hpp"
#include "sc/communication/iec61850/IecReferences.hpp"
#include "sc/communication/iec61850/IEC61850Manager.hpp"
#include "sc/communication/attack/AttackInterface.hpp"
#include "sc/communication/CommunicationConfig.hpp"
#include "sc/runtime/Logging.hpp"
#include "sc/runtime/Time.hpp"

struct IECCommunicatorTestAccess;

class IECCommunicator
{
public:
    explicit IECCommunicator(const CommunicationConfig& config,
                             int turbineId,
                             IEC61850Manager& iecManager,
                             AttackInterface::AttackInterface& attackInterface);
    ~IECCommunicator();

    bool start();
    void stop();
    void setFailureHandler(PeriodicTask::FailureHandler handler);

    int turbineId() const { return turbineId_; }
    CommunicationStatus status() const { return iecStatus_.load(); }
    std::chrono::system_clock::time_point lastActivityTime() const;

private:
    friend struct IECCommunicatorTestAccess;

    class RxTask : public PeriodicTask
    {
    public:
        RxTask(IECCommunicator& owner, std::chrono::milliseconds period)
            : PeriodicTask(period),
              owner_(owner)
        {
        }

    private:
        void execute() override { owner_.executeRx(); }

        IECCommunicator& owner_;
    };

    class TxTask : public PeriodicTask
    {
    public:
        TxTask(IECCommunicator& owner, std::chrono::milliseconds period)
            : PeriodicTask(period),
              owner_(owner)
        {
        }

    private:
        void execute() override { owner_.executeTx(); }

        IECCommunicator& owner_;
    };

    enum class TxValueType {
        Float,
        Unsigned,
    };

    struct RxDescriptor {
        const char*                              name;
        const char*                              unit;
        const char*                              daReference;
        const char*                              reportReference;
        AttackInterface::SignalType              txDataType;
        std::vector<double> CollectedData::*     lastField;
        TurbineHistory<double> CollectedData::*  historyField;
        std::vector<uint64_t> CollectedData::*   lastTimestamp;
        uint32_t                                 intervalMs;
    };

    struct TxDescriptor {
        const char*                                  name;
        TxValueType                                  type;
        std::function<void*(ControlData&, int)>      valuePtr;
        AttackInterface::SignalType                  txDataType;
        const char*                                  controlReference;
        const char*                                  stateReference;
        uint32_t                                     intervalMs;
    };

    static std::string descToString(void* value, const TxDescriptor& desc);
    uint64_t getRxNextExecutionTimeMs(size_t index) const;
    uint64_t getTxNextExecutionTimeMs(size_t index) const;
    void setRxNextExecutionTimeMs(size_t index, uint64_t timeMs);
    void setTxNextExecutionTimeMs(size_t index, uint64_t timeMs);

    void executeRx();
    void executeTx();
    void recordSuccessfulCommunication();
    void updateConnectionStatus();
    void doTxSetpoint(size_t idx, const TxDescriptor& desc);
    void doRxMeasurement(size_t idx, const RxDescriptor& desc);
    void processRxMeasurement(const RxDescriptor& desc, float value, uint64_t timestampMs);
    void doRxSecret();
    bool reportingEnabled() const;
    void startReporting();
    void stopReporting();
    void handleReportValues(int turbineId, const std::vector<IecReportValue>& values);
    void handleWorkerFailure(const char* workerName, const std::string& message);
    std::vector<std::string> reportFallbackReferences() const;
    std::optional<size_t> findRxDescriptorByReference(const std::string& reference) const;

    const CommunicationConfig& config_;
    int turbineId_;
    IEC61850Manager& iecManager_;
    AttackInterface::AttackInterface& attackInterface_;

    std::atomic<CommunicationStatus> iecStatus_{COMM_DISCONNECTED};
    std::chrono::system_clock::time_point lastActivityTime_;
    mutable std::mutex lastActivityTimeMutex_;
    RxTask rxTask_;
    TxTask txTask_;

    std::vector<uint64_t> rxNextExecutionTimes_;    ///< next execution times for RX descriptors
    std::vector<uint64_t> txNextExecutionTimes_;    ///< next execution times for TX descriptors
    struct BufferedRxMeasurement {
        float value {0.0f};
        uint64_t timestampMs {0};
    };
    std::vector<std::optional<BufferedRxMeasurement>> reportRxBuffer_;
    std::mutex reportRxBufferMutex_;
    std::atomic<bool> reportStarted_ {false};
    bool started_ {false};
    std::mutex lifecycleMutex_;
    PeriodicTask::FailureHandler failureHandler_;
    std::mutex failureHandlerMutex_;

    static const RxDescriptor RX_DESCRIPTORS[];
    static const TxDescriptor TX_DESCRIPTORS[];
};
