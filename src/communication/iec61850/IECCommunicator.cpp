#include "sc/communication/iec61850/IECCommunicator.hpp"
#include "sc/runtime/Logging.hpp"
#include "sc/runtime/Time.hpp"

#include <cstring>
#include <algorithm>
#include <utility>

const IECCommunicator::RxDescriptor IECCommunicator::RX_DESCRIPTORS[] = {
    { "V", "m/s", IEC_STRINGS::WS_MEAS, "WMET1$MX$HorWdSpd", AttackInterface::SignalType::WIND_SPEED, &CollectedData::lastWS, &CollectedData::wsHistory, &CollectedData::lastWS_t, 500 },
    { "D", "deg", IEC_STRINGS::WD_MEAS, "WMET1$MX$HorWdDir", AttackInterface::SignalType::WIND_DIRECTION, &CollectedData::lastWD, &CollectedData::wdHistory, &CollectedData::lastWD_t, 500 },
    { "YawMeas", "deg", IEC_STRINGS::YAW_MEAS, "WYAW1$MX$YwAng", AttackInterface::SignalType::YAW_ANGLE, &CollectedData::lastYawOffset, &CollectedData::yawOffsetHistory, &CollectedData::lastYawOffset_t, 500 },
    { "RSpd", "RPM", IEC_STRINGS::RPM_MEAS, "WROT1$MX$RotSpd", AttackInterface::SignalType::ROTOR_SPEED, &CollectedData::lastRPM, &CollectedData::rpmHistory, &CollectedData::lastRPM_t, 500 },
    { "W", "W", IEC_STRINGS::POWER_MEAS, "WTUR1$MX$W", AttackInterface::SignalType::POWER, &CollectedData::lastPower, &CollectedData::powerHistory, &CollectedData::lastPower_t, 500 },
    { "Tor", "Nm", IEC_STRINGS::GEN_TORQ, "WCNV1$MX$Torq", AttackInterface::SignalType::GENERATOR_TORQUE, &CollectedData::lastGenTorque, &CollectedData::genTorqueHistory, &CollectedData::lastGenTorque_t, 500 },
};

const IECCommunicator::TxDescriptor IECCommunicator::TX_DESCRIPTORS[] = {
    { "WSpt", TxValueType::Float, [](ControlData& d, int i)->void* { return &d.powerSetpoints[i]; }, AttackInterface::SignalType::POWER_SETPOINT, IEC_STRINGS::WTUR_DmdWSpt, nullptr, 1000 },
    { "YawSpt", TxValueType::Float, [](ControlData& d, int i)->void* { return &d.yawSetpoints[i]; }, AttackInterface::SignalType::YAW_SETPOINT, IEC_STRINGS::XWYAW_YawSpt, nullptr, 1000 },
    { "OP_CMD", TxValueType::Unsigned, [](ControlData& d, int i)->void* { return &d.turbineEnabled[i]; }, AttackInterface::SignalType::NONE, IEC_STRINGS::WTUR_OP_CMD, IEC_STRINGS::WTUR_OP_CMD_VAL, 5000 },
    { "TUR_CTL", TxValueType::Unsigned, [](ControlData& d, int i)->void* { return &d.turbineController[i]; }, AttackInterface::SignalType::NONE, IEC_STRINGS::WTUR_TURCTL, IEC_STRINGS::WTUR_TURCTL_VAL, 5000 },
};

IECCommunicator::IECCommunicator(const CommunicationConfig& config,
                                 int turbineId,
                                 IEC61850Manager& iecManager,
                                 AttackInterface::AttackInterface& attackInterface)
    : config_(config),
      turbineId_(turbineId),
      iecManager_(iecManager),
      attackInterface_(attackInterface),
      lastActivityTime_(),
      rxTask_(*this, config.mms.pollPeriod),
      txTask_(*this, config.mms.pollPeriod),
      rxNextExecutionTimes_(std::size(RX_DESCRIPTORS), 0),
      txNextExecutionTimes_(std::size(TX_DESCRIPTORS), 0),
      reportRxBuffer_(std::size(RX_DESCRIPTORS))
{
    rxTask_.setFailureHandler([this](const std::string& message) {
        handleWorkerFailure("RX", message);
    });
    txTask_.setFailureHandler([this](const std::string& message) {
        handleWorkerFailure("TX", message);
    });
}

IECCommunicator::~IECCommunicator()
{
    stop();
}

std::chrono::system_clock::time_point IECCommunicator::lastActivityTime() const
{
    std::lock_guard<std::mutex> lock(lastActivityTimeMutex_);
    return lastActivityTime_;
}

bool IECCommunicator::start()
{
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (started_) {
        return false;
    }
    iecStatus_.store(COMM_CONNECTING);
    iecManager_.connectTurbine(turbineId_);
    startReporting();
    if (!rxTask_.start()) {
        stopReporting();
        iecStatus_.store(COMM_DISCONNECTED);
        return false;
    }
    if (!txTask_.start()) {
        rxTask_.stop();
        stopReporting();
        iecStatus_.store(COMM_DISCONNECTED);
        return false;
    }
    if (!rxTask_.isRunning() || !txTask_.isRunning()) {
        txTask_.stop();
        rxTask_.stop();
        stopReporting();
        iecStatus_.store(COMM_DISCONNECTED);
        return false;
    }
    started_ = true;
    updateConnectionStatus();
    return true;
}

void IECCommunicator::stop()
{
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    txTask_.stop();
    rxTask_.stop();
    stopReporting();
    iecManager_.disconnectTurbine(turbineId_);
    started_ = false;
    iecStatus_.store(COMM_DISCONNECTED);
}

void IECCommunicator::setFailureHandler(PeriodicTask::FailureHandler handler)
{
    std::lock_guard<std::mutex> lock(failureHandlerMutex_);
    failureHandler_ = std::move(handler);
}

void IECCommunicator::handleWorkerFailure(const char* workerName, const std::string& message)
{
    iecStatus_.store(COMM_DISCONNECTED);
    PeriodicTask::FailureHandler handler;
    {
        std::lock_guard<std::mutex> lock(failureHandlerMutex_);
        handler = failureHandler_;
    }
    if (handler) {
        handler("IEC turbine " + std::to_string(turbineId_) + " " + workerName +
                " worker: " + message);
    }
}

void IECCommunicator::executeTx()
{
    const uint64_t currentTimeMs = getCurrentTimeMs();
    for (size_t i = 0; i < std::size(TX_DESCRIPTORS); ++i) {
        if (currentTimeMs >= getTxNextExecutionTimeMs(i)) {
            doTxSetpoint(static_cast<size_t>(i), TX_DESCRIPTORS[i]);
            setTxNextExecutionTimeMs(i, getCurrentTimeMs() + TX_DESCRIPTORS[i].intervalMs);
        }
    }
}

void IECCommunicator::executeRx()
{
    const uint64_t currentTimeMs = getCurrentTimeMs();
    if (reportingEnabled()) {
        reportStarted_.store(iecManager_.periodicReportActive(
            turbineId_, config_.mms.reportControlBlockReference));
    }
    updateConnectionStatus();

    if (reportingEnabled() && reportStarted_.load()) {
        std::vector<std::optional<BufferedRxMeasurement>> reportValues;
        {
            std::lock_guard<std::mutex> lock(reportRxBufferMutex_);
            reportValues.swap(reportRxBuffer_);
            reportRxBuffer_.resize(std::size(RX_DESCRIPTORS));
        }

        for (size_t i = 0; i < reportValues.size(); ++i) {
            if (reportValues[i]) {
				COMMTASK_LOG_V2("IEComm[" << turbineId_ << "] Processing buffered report value for " << RX_DESCRIPTORS[i].name << ": " << reportValues[i]->value << " " << RX_DESCRIPTORS[i].unit);
                processRxMeasurement(RX_DESCRIPTORS[i], reportValues[i]->value, reportValues[i]->timestampMs);
            }
        }
    } else {
        for (size_t i = 0; i < std::size(RX_DESCRIPTORS); ++i) {
            if (currentTimeMs >= getRxNextExecutionTimeMs(i)) {
                doRxMeasurement(static_cast<size_t>(i), RX_DESCRIPTORS[i]);
                setRxNextExecutionTimeMs(i, currentTimeMs + RX_DESCRIPTORS[i].intervalMs);
            }
        }
    }
}

std::string IECCommunicator::descToString(void* value, const TxDescriptor& desc)
{
    switch (desc.type) {
        case TxValueType::Float: return std::to_string(*static_cast<float*>(value));
        case TxValueType::Unsigned: return std::to_string(*static_cast<uint32_t*>(value));
    }
    return "unknown";
}

uint64_t IECCommunicator::getRxNextExecutionTimeMs(size_t index) const
{
    return rxNextExecutionTimes_[index];
}

uint64_t IECCommunicator::getTxNextExecutionTimeMs(size_t index) const
{
    return txNextExecutionTimes_[index];
}

void IECCommunicator::setRxNextExecutionTimeMs(size_t index, uint64_t timeMs)
{
    rxNextExecutionTimes_[index] = timeMs;
}

void IECCommunicator::setTxNextExecutionTimeMs(size_t index, uint64_t timeMs)
{
    txNextExecutionTimes_[index] = timeMs;
}

void IECCommunicator::recordSuccessfulCommunication()
{
    std::lock_guard<std::mutex> lock(lastActivityTimeMutex_);
    lastActivityTime_ = std::chrono::system_clock::now();
}

void IECCommunicator::updateConnectionStatus()
{
    switch (iecManager_.status(turbineId_)) {
        case IEC_LINK_CONNECTED: iecStatus_.store(COMM_CONNECTED); break;
        case IEC_LINK_CONNECTING:
        case IEC_LINK_RECONNECTING: iecStatus_.store(COMM_CONNECTING); break;
        case IEC_LINK_CLOSED:
        case IEC_LINK_ERROR: iecStatus_.store(COMM_DISCONNECTED); break;
    }
}

void IECCommunicator::doTxSetpoint(size_t /*idx*/, const TxDescriptor& desc)
{
    float floatValue = 0.0f;
    uint32_t uintValue = 0;
    void* value = nullptr;
    {
        auto& control = SharedData::instance().control;
        std::lock_guard<std::mutex> lock(control.mutex);
        void* sharedValue = desc.valuePtr(control, turbineId_ - 1);
        switch (desc.type) {
            case TxValueType::Float:
                floatValue = *static_cast<float*>(sharedValue);
                value = &floatValue;
                break;
            case TxValueType::Unsigned:
                uintValue = *static_cast<uint32_t*>(sharedValue);
                value = &uintValue;
                break;
        }
    }

    std::string logMsg = "[SC→WT" + std::to_string(turbineId_) + "]" + std::to_string(getCurrentTimeMs()) + ";" + desc.name + "=" + descToString(value, desc);
    DataHistorian::instance().log(logMsg);

    if (desc.type == TxValueType::Float) {
        attackInterface_.processValue(turbineId_, desc.txDataType, floatValue);
    } else {
        attackInterface_.processValue(turbineId_, desc.txDataType, uintValue);
    }

    logMsg = "[SC→WT" + std::to_string(turbineId_) + "(A)]" + std::to_string(getCurrentTimeMs()) + ";" + desc.name + "=" + descToString(value, desc);
    DataHistorian::instance().log(logMsg);

    bool writeSucceeded = false;
    const std::string controlReference = iecManager_.buildRef(turbineId_, desc.controlReference);
    if (desc.type == TxValueType::Float) {
        writeSucceeded = iecManager_.writeControlledFloat(
            turbineId_, controlReference, *static_cast<float*>(value), false);
    } else if (desc.type == TxValueType::Unsigned && desc.stateReference != nullptr) {
        const int requested = static_cast<int>(*static_cast<uint32_t*>(value));
        const auto current = iecManager_.readInt(
            turbineId_, iecManager_.buildRef(turbineId_, desc.stateReference), 0);
        if (current) recordSuccessfulCommunication();
        writeSucceeded = current && *current == requested;
        if (!writeSucceeded) {
            writeSucceeded = iecManager_.writeControlledEnum(
                turbineId_, controlReference, requested, false);
        }
    }

    if (!writeSucceeded) {
        if (iecManager_.status(turbineId_) == IEC_LINK_CONNECTED) {
            COMMTASK_ERR("Failed to write " << desc.name << " to turbine " << turbineId_);
        } else {
            COMMTASK_LOG_V2("Deferred " << desc.name << " for reconnecting turbine " << turbineId_);
        }
    } else {
        recordSuccessfulCommunication();
        COMMTASK_LOG_V1("Sent " << desc.name << " to turbine " << turbineId_ << ": " << descToString(value, desc));
    }
    updateConnectionStatus();
}

void IECCommunicator::doRxMeasurement(size_t /*idx*/, const RxDescriptor& desc)
{
    const auto value = iecManager_.readFloat(
        turbineId_, iecManager_.buildRef(turbineId_, desc.daReference), 1);
    if (!value) {
        if (iecManager_.status(turbineId_) == IEC_LINK_CONNECTED) {
            COMMTASK_ERR("Failed to read " << desc.name << " from turbine " << turbineId_);
        } else {
            COMMTASK_LOG_V2("No " << desc.name << " while turbine " << turbineId_ << " reconnects");
        }
        updateConnectionStatus();
        return;
    }

    recordSuccessfulCommunication();
    updateConnectionStatus();
    processRxMeasurement(desc, *value, getCurrentTimeMs());
}

void IECCommunicator::processRxMeasurement(const RxDescriptor& desc, float value, uint64_t timestampMs)
{
    COMMTASK_LOG_V2("Received (pre-overwrite) " << desc.name << " from turbine " << turbineId_ << ": " << value << " " << desc.unit);
    std::string logMsg = "[WT" + std::to_string(turbineId_) + "→SC]" + std::to_string(timestampMs) + ";" + desc.name + "=" + std::to_string(value);
    DataHistorian::instance().log(logMsg);
    
    
    if (strcmp(desc.name, "W") == 0) { // If we receive power, we also store the actual value in order to keep track of total measured power
            auto& collected = SharedData::instance().collected;
            std::lock_guard<std::mutex> lock(collected.mutex);
            collected.measuredPower[turbineId_ - 1] = value;
    }
    attackInterface_.processValue(turbineId_, desc.txDataType, value);

    COMMTASK_LOG_V1("Received (post-overwrite) " << desc.name << " for turbine " << turbineId_ << ": " << value << " " << desc.unit);
    logMsg = "[WT" + std::to_string(turbineId_) + "→SC(A)]" + std::to_string(getCurrentTimeMs()) + ";" + desc.name + "=" + std::to_string(value);
    DataHistorian::instance().log(logMsg);

    bool unchangedWindDirection = false;
    double previousWindDirection = 0.0;
    {
        auto& collected = SharedData::instance().collected;
        std::lock_guard<std::mutex> lock(collected.mutex);

        if (strcmp(desc.name, "D") == 0) {
            previousWindDirection = (collected.*desc.lastField)[turbineId_ - 1];
            unchangedWindDirection = previousWindDirection == value;
        }

        (collected.*desc.lastField)[turbineId_ - 1] = value;
        (collected.*desc.historyField)[turbineId_ - 1].push_back(value);
        (collected.*desc.lastTimestamp)[turbineId_ - 1] = timestampMs;
    }
    if (unchangedWindDirection) {
        COMMTASK_ST("Wind direction did not change for turbine " << turbineId_ << ": "
                    << previousWindDirection << " -> " << value);
    }
}

bool IECCommunicator::reportingEnabled() const
{
    return config_.mms.reportingEnabled &&
           !config_.mms.reportControlBlockReference.empty();
}

void IECCommunicator::startReporting()
{
    if (!reportingEnabled())
        return;

    const uint32_t periodMs = static_cast<uint32_t>(config_.mms.reportTriggerPeriod.count());
    if (periodMs == 0) {
        COMMTASK_ERR("IEC reporting enabled but reportTriggerPeriod is zero");
        return;
    }

    auto callback = [this](int turbineId, const std::vector<IecReportValue>& values) {
        handleReportValues(turbineId, values);
    };

    if (iecManager_.startPeriodicReport(turbineId_,
                                        config_.mms.reportControlBlockReference,
                                        config_.mms.reportDataSetReference,
                                        periodMs,
                                        reportFallbackReferences(),
                                        callback)) {
        reportStarted_.store(true);
        COMMTASK_ST("Enabled IEC report input for turbine " << turbineId_);
    } else {
        reportStarted_.store(false);
        COMMTASK_ST("IEC report input pending for turbine " << turbineId_ << "; using polling");
    }
}

void IECCommunicator::stopReporting()
{
    if (!reportingEnabled())
        return;

    iecManager_.stopPeriodicReport(turbineId_, config_.mms.reportControlBlockReference);
    reportStarted_.store(false);
}

void IECCommunicator::handleReportValues(int turbineId, const std::vector<IecReportValue>& values)
{
    if (turbineId != turbineId_) {
        COMMTASK_ERR("Received IEC report for turbine " << turbineId
                     << " in communicator for turbine " << turbineId_);
        return;
    }

    bool accepted = false;
    std::lock_guard<std::mutex> lock(reportRxBufferMutex_);
    for (const auto& value : values) {
        const auto index = findRxDescriptorByReference(value.reference);
        if (index) {
            reportRxBuffer_[*index] = BufferedRxMeasurement{value.value, value.timestampMs};
            accepted = true;
        } else {
            COMMTASK_LOG_V2("Ignoring unmapped IEC report value for turbine " << turbineId
                            << ": ref=" << value.reference << ", value=" << value.value);
        }
    }

    if (accepted) {
        recordSuccessfulCommunication();
        iecStatus_.store(COMM_CONNECTED);
    }

	//LIBIEC_ST("IECCommunicator[" << turbineId_ << "] Received IEC report for turbine " << turbineId << " with ");
}

std::vector<std::string> IECCommunicator::reportFallbackReferences() const
{
    if (!config_.mms.reportDataReferences.empty())
        return config_.mms.reportDataReferences;

    std::vector<std::string> refs;
    refs.reserve(std::size(RX_DESCRIPTORS));
    for (const auto& desc : RX_DESCRIPTORS)
        refs.push_back(desc.daReference);
    return refs;
}

std::optional<size_t> IECCommunicator::findRxDescriptorByReference(const std::string& reference) const
{
    auto endsWith = [](const std::string& value, const std::string& suffix) {
        return value.size() >= suffix.size() &&
               value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    for (size_t i = 0; i < std::size(RX_DESCRIPTORS); ++i) {
        if (reference == RX_DESCRIPTORS[i].name ||
            reference == RX_DESCRIPTORS[i].daReference ||
            reference == RX_DESCRIPTORS[i].reportReference ||
            endsWith(reference, RX_DESCRIPTORS[i].daReference) ||
            endsWith(reference, RX_DESCRIPTORS[i].reportReference)) {
            return i;
        }
    }

    return std::nullopt;
}

void IECCommunicator::doRxSecret()
{
    const auto secret = iecManager_.readString(
        turbineId_, iecManager_.buildRef(turbineId_, IEC_STRINGS::SECR_S), 0);
    if (secret) {
        recordSuccessfulCommunication();
        COMMTASK_LOG_V2("Received secret from turbine " << turbineId_ << ": " << *secret);
    } else {
        COMMTASK_ERR("Failed to read secret from turbine " << turbineId_);
    }
}
