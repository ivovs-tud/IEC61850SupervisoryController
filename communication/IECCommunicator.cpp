#include "IECCommunicator.hpp"
#include "common/config.hpp"
#include "common/util.hpp"
#include "common/ConsoleColors.hpp"

#include <cstring>

const IECCommunicator::RxDescriptor IECCommunicator::RX_DESCRIPTORS[] = {
    { "V",       "m/s", &libiec_wrapper::rxWindSpeed,    AttackInterface::TX_WS,  &GlobalData::lastWS,     &GlobalData::wsHistory,    &GlobalData::lastWS_t,     500 },
    { "D",       "deg", &libiec_wrapper::rxWindDirection, AttackInterface::TX_WD,  &GlobalData::lastWD,     &GlobalData::wdHistory,    &GlobalData::lastWD_t,     500 },
    { "YawMeas", "deg", &libiec_wrapper::rxYawOffset,    AttackInterface::TX_YAW, &GlobalData::lastYawOffset, &GlobalData::yawOffsetHistory, &GlobalData::lastYawOffset_t, 500 },
    { "RSpd",    "RPM", &libiec_wrapper::rxRotorSpeed,   AttackInterface::TX_RPM, &GlobalData::lastRPM,    &GlobalData::rpmHistory,   &GlobalData::lastRPM_t,    500 },
    { "W",       "W",   &libiec_wrapper::rxPowerGen,     AttackInterface::TX_PW,  &GlobalData::lastPower,   &GlobalData::powerHistory, &GlobalData::lastPower_t,   500 },
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
    : PeriodicTask(config.mms.pollPeriod),
      config_(config),
      turbineId_(turbineId),
      iecWrapper_(iecWrapper),
      attackInterface_(attackInterface),
      attackInterfaceMutex_(attackInterfaceMutex),
      lastActivityTime_(std::chrono::system_clock::now()),
      rxNextExecutionTimes_(std::size(RX_DESCRIPTORS), 0),
      txNextExecutionTimes_(std::size(TX_DESCRIPTORS), 0)
{
}

void IECCommunicator::onStart()
{
    iecStatus_.store(COMM_CONNECTING);
    iecStatus_.store(COMM_CONNECTED);
}

void IECCommunicator::execute()
{
    const uint64_t currentTimeMs = getCurrentTimeMs();
    lastActivityTime_ = std::chrono::system_clock::now();

    for (size_t i = 0; i < std::size(TX_DESCRIPTORS); ++i) {
        if (currentTimeMs >= getTxNextExecutionTimeMs(i)) {
            doTxSetpoint(static_cast<size_t>(i), TX_DESCRIPTORS[i]);
            setTxNextExecutionTimeMs(i, currentTimeMs + TX_DESCRIPTORS[i].intervalMs);
        }
    }

    for (size_t i = 0; i < std::size(RX_DESCRIPTORS); ++i) {
        if (currentTimeMs >= getRxNextExecutionTimeMs(i)) {
            doRxMeasurement(static_cast<size_t>(i), RX_DESCRIPTORS[i]);
            setRxNextExecutionTimeMs(i, currentTimeMs + RX_DESCRIPTORS[i].intervalMs);
        }
    }
}

void IECCommunicator::onStop()
{
    iecStatus_.store(COMM_DISCONNECTED);
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

void IECCommunicator::doTxSetpoint(size_t /*idx*/, const TxDescriptor& desc)
{
    void* value = nullptr;
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
            float& f = *static_cast<float*>(value);
            if (attackInterface_.overwrite(turbineId_, desc.txDataType, f) < 0) {
                COMMTASK_ERR("Failed to get overwrite decision for " << desc.name << " from turbine " << turbineId_);
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

    COMMTASK_LOG_V2("Received (pre-overwrite) " << desc.name << " from turbine " << turbineId_ << ": " << value << " " << desc.unit);
    std::string logMsg = "[WT" + std::to_string(turbineId_) + "→SC]" + std::to_string(getCurrentTimeMs()) + ";" + desc.name + "=" + std::to_string(value);
    DataHistorian::instance().log(logMsg);

    {
        std::lock_guard<std::mutex> lock(attackInterfaceMutex_);
        attackInterface_.txData(turbineId_, desc.txDataType, &value);

        if (attackInterface_.overwrite(turbineId_, desc.txDataType, value) < 0) {
            COMMTASK_ERR("Failed to get overwrite decision for " << desc.name << " from turbine " << turbineId_);
        }
    }

    if (strcmp(desc.name, "W") == 0) {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        GlobalDataStructure::instance().data()._W[turbineId_ - 1] = value;
    }

    COMMTASK_LOG_V1("Received (post-overwrite) " << desc.name << " for turbine " << turbineId_ << ": " << value << " " << desc.unit);
    logMsg = "[WT" + std::to_string(turbineId_) + "→SC(A)]" + std::to_string(getCurrentTimeMs()) + ";" + desc.name + "=" + std::to_string(value);
    DataHistorian::instance().log(logMsg);

    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        auto& gds = GlobalDataStructure::instance().data();
        (gds.*desc.lastField)[turbineId_ - 1] = value;
        (gds.*desc.historyField)[turbineId_ - 1].push_back(value);
        (gds.*desc.lastTimestamp)[turbineId_ - 1] = getCurrentTimeMs();
    }
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
