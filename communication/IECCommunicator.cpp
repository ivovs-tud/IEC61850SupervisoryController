#include "IECCommunicator.hpp"
#include "common/config.hpp"
#include "common/util.hpp"
#include "common/ConsoleColors.hpp"

#include <cstring>
#include <algorithm>

const IECCommunicator::RxDescriptor IECCommunicator::RX_DESCRIPTORS[] = {
    { "V",       "m/s", IEC_STRINGS::WS_MEAS,    "WMET1$MX$HorWdSpd", &libiec_wrapper::rxWindSpeed,     AttackInterface::TX_WS,  &GlobalData::lastWS,        &GlobalData::wsHistory,        &GlobalData::lastWS_t,        500 },
    { "D",       "deg", IEC_STRINGS::WD_MEAS,    "WMET1$MX$HorWdDir", &libiec_wrapper::rxWindDirection, AttackInterface::TX_WD,  &GlobalData::lastWD,        &GlobalData::wdHistory,        &GlobalData::lastWD_t,        500 },
    { "YawMeas", "deg", IEC_STRINGS::YAW_MEAS,   "WYAW1$MX$YwAng",    &libiec_wrapper::rxYawOffset,     AttackInterface::TX_YAW, &GlobalData::lastYawOffset, &GlobalData::yawOffsetHistory, &GlobalData::lastYawOffset_t, 500 },
    { "RSpd",    "RPM", IEC_STRINGS::RPM_MEAS,   "WROT1$MX$RotSpd",   &libiec_wrapper::rxRotorSpeed,    AttackInterface::TX_RPM, &GlobalData::lastRPM,       &GlobalData::rpmHistory,       &GlobalData::lastRPM_t,       500 },
    { "W",       "W",   IEC_STRINGS::POWER_MEAS, "WTUR1$MX$W",        &libiec_wrapper::rxPowerGen,      AttackInterface::TX_PW,  &GlobalData::lastPower,     &GlobalData::powerHistory,     &GlobalData::lastPower_t,     500 },
    { "Tor",     "W",   IEC_STRINGS::GEN_TORQ,   "WCNV1$MX$Torq",     &libiec_wrapper::rxGenTorque,     AttackInterface::TX_GENTORQ,  &GlobalData::lastGenTorque,     &GlobalData::genTorqueHistory,     &GlobalData::lastGenTorque_t,     500 },
};

const IECCommunicator::TxDescriptor IECCommunicator::TX_DESCRIPTORS[] = {
    { "WSpt",    IEC_FLOAT32, [](GlobalData& d, int i)->void* { return &d.TurbinePowerSetpoints[i]; }, AttackInterface::TX_SPT_PWR,    &libiec_wrapper::txPowerSetpoint,     5000 },
    { "YawSpt",  IEC_FLOAT32, [](GlobalData& d, int i)->void* { return &d.TurbineYawSetpoints[i]; },   AttackInterface::TX_SPT_YAW,    &libiec_wrapper::txYawSetpoint,       1000 },
    { "OP_CMD",  IEC_UINT32,  [](GlobalData& d, int i)->void* { return &d.enableTurbine[i]; },          AttackInterface::TX_NONE,       &libiec_wrapper::txOpCommand,         1000 },
    { "TUR_CTL", IEC_UINT32,  [](GlobalData& d, int i)->void* { return &d.TurbineController[i]; },     AttackInterface::TX_NONE,       &libiec_wrapper::txTurbineController, 1000 },
};

IECCommunicator::IECCommunicator(const CommConfig& config,
                                 int turbineId,
                                 libiec_wrapper& iecWrapper,
                                 AttackInterface::AttackInterface& attackInterface,
                                 std::mutex& attackInterfaceMutex)
    : config_(config),
      turbineId_(turbineId),
      iecWrapper_(iecWrapper),
      attackInterface_(attackInterface),
      attackInterfaceMutex_(attackInterfaceMutex),
      lastActivityTime_(std::chrono::system_clock::now()),
      rxTask_(*this, config.mms.pollPeriod),
      txTask_(*this, config.mms.pollPeriod),
      rxNextExecutionTimes_(std::size(RX_DESCRIPTORS), 0),
      txNextExecutionTimes_(std::size(TX_DESCRIPTORS), 0),
      reportRxBuffer_(std::size(RX_DESCRIPTORS))
{
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

void IECCommunicator::start()
{
    iecStatus_.store(COMM_CONNECTING);
    startReporting();
    rxTask_.start();
    txTask_.start();
    iecStatus_.store(COMM_CONNECTED);
}

void IECCommunicator::stop()
{
    stopReporting();
    txTask_.stop();
    rxTask_.stop();
    iecStatus_.store(COMM_DISCONNECTED);
}

void IECCommunicator::executeTx()
{
    const uint64_t currentTimeMs = getCurrentTimeMs();
    touchActivityTime();

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
    touchActivityTime();

    if (reportingEnabled() && reportStarted_.load()) {
        std::vector<std::optional<BufferedRxMeasurement>> reportValues;
        {
            std::lock_guard<std::mutex> lock(reportRxBufferMutex_);
            reportValues.swap(reportRxBuffer_);
            reportRxBuffer_.resize(std::size(RX_DESCRIPTORS));
        }

        for (size_t i = 0; i < reportValues.size(); ++i) {
            if (reportValues[i]) {
				COMMTASK_ST("IEComm[" << turbineId_ << "] Processing buffered report value for " << RX_DESCRIPTORS[i].name << ": " << reportValues[i]->value << " " << RX_DESCRIPTORS[i].unit);
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
        case IEC_FLOAT32: return std::to_string(*static_cast<float*>(value));
        case IEC_INT32:   return std::to_string(*static_cast<int*>(value));
        case IEC_UINT32:  return std::to_string(*static_cast<uint32_t*>(value));
        case IEC_BOOL:    return *static_cast<bool*>(value) ? "True" : "False";
        default:          return "unknown";
    }
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

void IECCommunicator::touchActivityTime()
{
    std::lock_guard<std::mutex> lock(lastActivityTimeMutex_);
    lastActivityTime_ = std::chrono::system_clock::now();
}

void IECCommunicator::doTxSetpoint(size_t /*idx*/, const TxDescriptor& desc)
{
    void* value = nullptr;
    float f;
    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        value = desc.gdsPtr(GlobalDataStructure::instance().data(), turbineId_ - 1);
    }

    std::string logMsg = "[SC→WT" + std::to_string(turbineId_) + "]" + std::to_string(getCurrentTimeMs()) + ";" + descToString(value, desc);
    DataHistorian::instance().log(logMsg);

    {
        std::lock_guard<std::mutex> lock(attackInterfaceMutex_);
        attackInterface_.txData(turbineId_, desc.txDataType, value);

        if (desc.type == IEC_FLOAT32) {
            //float& f = *static_cast<float*>(value);
            auto bOverwrite = attackInterface_.overwrite(turbineId_, desc.txDataType, f);
            if (bOverwrite < 0) {
                COMMTASK_ERR("Failed to get overwrite decision for " << desc.name << " from turbine " << turbineId_);
            } else if (bOverwrite > 0) { // Meaning we succefully overwritten
                value = &f;     // We do this pointer trick here to make sure the actual value is not overwritten
            }            
        }
    }

    logMsg = "[SC→WT" + std::to_string(turbineId_) + "(A)]" + std::to_string(getCurrentTimeMs()) + ";" + descToString(value, desc);
    DataHistorian::instance().log(logMsg);

    if ((iecWrapper_.*desc.iecWrite)(turbineId_, value) != IEC_OK) {
        COMMTASK_ERR("Failed to write " << desc.name << " to turbine " << turbineId_);
    } else {
        COMMTASK_LOG_V1("Sent " << desc.name << " to turbine " << turbineId_ << ": " << descToString(value, desc));
    }
}

void IECCommunicator::doRxMeasurement(size_t /*idx*/, const RxDescriptor& desc)
{
    float value = 0.0f;
    if ((iecWrapper_.*desc.iecRead)(turbineId_, value) != IEC_OK) {
        COMMTASK_ERR("Failed to read " << desc.name << " from turbine " << turbineId_);
        return;
    }

    processRxMeasurement(desc, value, getCurrentTimeMs());
}

void IECCommunicator::processRxMeasurement(const RxDescriptor& desc, float value, uint64_t timestampMs)
{
    COMMTASK_LOG_V2("Received (pre-overwrite) " << desc.name << " from turbine " << turbineId_ << ": " << value << " " << desc.unit);
    std::string logMsg = "[WT" + std::to_string(turbineId_) + "→SC]" + std::to_string(timestampMs) + ";" + desc.name + "=" + std::to_string(value);
    DataHistorian::instance().log(logMsg);
    
    
    if (strcmp(desc.name, "W") == 0) { // If we receive power, we also store the actual value in order to keep track of total measured power
            std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
            GlobalDataStructure::instance().data()._W[turbineId_ - 1] = value;
    }
    {
        std::lock_guard<std::mutex> lock(attackInterfaceMutex_);
        attackInterface_.txData(turbineId_, desc.txDataType, &value);

        if (attackInterface_.overwrite(turbineId_, desc.txDataType, value) < 0) {
            COMMTASK_ERR("Failed to get overwrite decision for " << desc.name << " from turbine " << turbineId_);
        }
    }

    COMMTASK_LOG_V1("Received (post-overwrite) " << desc.name << " for turbine " << turbineId_ << ": " << value << " " << desc.unit);
    logMsg = "[WT" + std::to_string(turbineId_) + "→SC(A)]" + std::to_string(getCurrentTimeMs()) + ";" + desc.name + "=" + std::to_string(value);
    DataHistorian::instance().log(logMsg);

    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        auto& gds = GlobalDataStructure::instance().data();

        // For debuggini
        if(strcmp(desc.name, "D") == 0) {
            if((gds.*desc.lastField)[turbineId_ - 1] == value) {
                COMMTASK_ST("Wind direction did not change for turbine " << turbineId_ << ": " << (gds.*desc.lastField)[turbineId_ - 1] << " -> " << value);
			}
		}

        (gds.*desc.lastField)[turbineId_ - 1] = value;
        (gds.*desc.historyField)[turbineId_ - 1].push_back(value);
        (gds.*desc.lastTimestamp)[turbineId_ - 1] = timestampMs;
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

    if (iecWrapper_.startPeriodicReport(turbineId_,
                                        config_.mms.reportControlBlockReference,
                                        config_.mms.reportDataSetReference,
                                        periodMs,
                                        reportFallbackReferences(),
                                        callback) == IEC_OK) {
        reportStarted_.store(true);
        COMMTASK_ST("Enabled IEC report input for turbine " << turbineId_);
    } else {
        reportStarted_.store(false);
        COMMTASK_ERR("Failed to enable IEC report input for turbine " << turbineId_ << "; falling back to polling");
    }
}

void IECCommunicator::stopReporting()
{
    if (!reportStarted_.load())
        return;

    iecWrapper_.stopPeriodicReport(turbineId_, config_.mms.reportControlBlockReference);
    reportStarted_.store(false);
}

void IECCommunicator::handleReportValues(int turbineId, const std::vector<IecReportValue>& values)
{
    if (turbineId != turbineId_) {
        COMMTASK_ERR("Received IEC report for turbine " << turbineId
                     << " in communicator for turbine " << turbineId_);
    }

    std::lock_guard<std::mutex> lock(reportRxBufferMutex_);
    for (const auto& value : values) {
        const auto index = findRxDescriptorByReference(value.reference);
        if (index) {
            reportRxBuffer_[*index] = BufferedRxMeasurement{value.value, value.timestampMs};
        } else {
            COMMTASK_LOG_V2("Ignoring unmapped IEC report value for turbine " << turbineId
                            << ": ref=" << value.reference << ", value=" << value.value);
        }
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
    std::string secret;
    if (iecWrapper_.rxSecret(turbineId_, secret) == IEC_OK) {
        COMMTASK_LOG_V2("Received secret from turbine " << turbineId_ << ": " << secret);
    } else {
        COMMTASK_ERR("Failed to read secret from turbine " << turbineId_);
    }
}
