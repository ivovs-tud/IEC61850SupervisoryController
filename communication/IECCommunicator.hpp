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

class IECCommunicator
{
public:
    explicit IECCommunicator(const CommConfig& config,
                             int turbineId,
                             libiec_wrapper& iecWrapper,
                             AttackInterface::AttackInterface& attackInterface,
                             std::mutex& attackInterfaceMutex);
    ~IECCommunicator();

    void start();
    void stop();

    int turbineId() const { return turbineId_; }
    CommStatus status() const { return iecStatus_.load(); }
    std::chrono::system_clock::time_point lastActivityTime() const;

private:
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

    typedef enum eIECValueType {
        IEC_FLOAT32,
        IEC_INT32,
        IEC_UINT32,
        IEC_BOOL,
        IEC_ENUM,
    } IECValueType;

    struct RxDescriptor {
        const char*                              name;
        const char*                              unit;
        const char*                              daReference;
        const char*                              reportReference;
        IECReturnCode (libiec_wrapper::*iecRead)(int, float&);
        AttackInterface::TxDataType              txDataType;
        std::vector<double> GlobalData::*        lastField;
        TurbineHistory<double> GlobalData::*     historyField;
        std::vector<uint64_t> GlobalData::*      lastTimestamp;
        uint32_t                                 intervalMs;
    };

    struct TxDescriptor {
        const char*                                  name;
        IECValueType                                 type;
        std::function<void*(GlobalData&, int)>       gdsPtr;
        AttackInterface::TxDataType                  txDataType;
        IECReturnCode (libiec_wrapper::*iecWrite)(int, void*);
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
    libiec_wrapper& iecWrapper_;
    AttackInterface::AttackInterface& attackInterface_;
    std::mutex& attackInterfaceMutex_;

    std::atomic<CommStatus> iecStatus_{COMM_DISCONNECTED};
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

    static const RxDescriptor RX_DESCRIPTORS[];
    static const TxDescriptor TX_DESCRIPTORS[];
};
