#include "libiec_wrapper.hpp"
#include <iostream>

extern "C" {
#include <iec61850_client.h>
#include <goose_receiver.h>
#include <goose_subscriber.h>
}


// ── init ─────────────────────────────────────────────────────────────────

IECReturnCode libiec_wrapper::init(const std::vector<TurbineEndpoint>& turbines, std::string networkInterface)
{
    if (turbines.empty()) {
        std::cerr << "[libiec_wrapper] init(): turbines vector is empty\n";
        return IEC_ERROR;
    }
    // Turbine IDs are 1-based; turbines[i] maps to turbine ID i+1.
    for (int i = 0; i < static_cast<int>(turbines.size()); ++i)
        manager_.addTurbine(i + 1, turbines[i].host, turbines[i].port, turbines[i].logicalDevice, turbines[i].iedName);

    // GOOSE Creation
    for (int i = 0; i < static_cast<int>(turbines.size()); ++i) {
        GooseReceiver receiver = GooseReceiver_create();
        GooseReceiver_setInterfaceId(receiver, networkInterface.c_str());
    }

    std::cout << "[libiec_wrapper] registered " << turbines.size() << " turbine(s)\n";
    return IEC_OK;
}

// ── start / stop ─────────────────────────────────────────────────────────

void libiec_wrapper::start() { 
    // manager_.connectAll(); 
    manager_.connectTurbine(1);
}
void libiec_wrapper::stop()  { manager_.disconnectAll(); }

// ── txSetpoint ───────────────────────────────────────────────────────────

IECReturnCode libiec_wrapper::txSetpoint(int turbineId, float powerSetpoint, int yawSetpoint) {
    bool ok = true;
    ok &= manager_.writeControlledFloat(turbineId, manager_.buildRef(turbineId, IEC_STRINGS::WTUR_DmdWSpt), powerSetpoint, false);
    ok &= manager_.writeControlledFloat(turbineId, manager_.buildRef(turbineId, IEC_STRINGS::XWYAW_YawSpt), yawSetpoint, false);
    return ok ? IEC_OK : IEC_ERROR;

}

IECReturnCode libiec_wrapper::rxSecret(int turbineId, std::string& outSecret) {
    auto secret = manager_.readString(turbineId, manager_.buildRef(turbineId, IEC_STRINGS::SECR_S), IEC61850_FC_ST);
    if (secret) {
        outSecret = *secret;
        return IEC_OK;
    }
    return IEC_ERROR;
}

IECReturnCode libiec_wrapper::rxWindSpeed(int turbineId, float& outWindSpeed) {
    auto ws = manager_.readFloat(turbineId, manager_.buildRef(turbineId, IEC_STRINGS::WS_MEAS), IEC61850_FC_MX);
    if (ws) {
        outWindSpeed = *ws;
        return IEC_OK;
    }
    return IEC_ERROR;
}

IECReturnCode libiec_wrapper::rxWindDirection(int turbineId, float& outWindDirection) {
    auto wd = manager_.readFloat(turbineId, manager_.buildRef(turbineId, IEC_STRINGS::WD_MEAS), IEC61850_FC_MX);
    if (wd) {
        outWindDirection = *wd;
        return IEC_OK;
    }
    return IEC_ERROR;
}

IECReturnCode libiec_wrapper::rxRotorSpeed(int turbineId, float& outRPM) {
    auto rpm = manager_.readFloat(turbineId, manager_.buildRef(turbineId, IEC_STRINGS::RPM_MEAS), IEC61850_FC_MX);
    if (rpm) {
        outRPM = *rpm;
        return IEC_OK;
    }
    return IEC_ERROR;
}

// ── checkTurbineSupport ──────────────────────────────────────────────────────

std::map<std::string, bool> libiec_wrapper::checkTurbineSupport(
    int turbineId,
    const std::vector<std::string>& references,
    int fc)
{
    std::vector<std::string> fullRefs;
    fullRefs.reserve(references.size());
    for (const auto& r : references)
        fullRefs.push_back(manager_.buildRef(turbineId, r));
    return manager_.checkSupported(turbineId, fullRefs, fc);
}

// ── printTurbineDataModel ────────────────────────────────────────────────────

void libiec_wrapper::printTurbineDataModel(int turbineId, int maxEntries)
{
    manager_.printDataModel(turbineId, maxEntries);
}

std::vector<std::string> libiec_wrapper::getTurbineDataModel(int turbineId)
{
    return manager_.getDataModelReferences(turbineId);
}
