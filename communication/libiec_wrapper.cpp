#include "libiec_wrapper.hpp"
#include "common/config.hpp"
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
        LIBIEC_ERR("init(): turbines vector is empty");
        return IEC_ERROR;
    }
    // Turbine IDs are 1-based; turbines[i] maps to turbine ID i+1.
    for (int i = 0; i < static_cast<int>(turbines.size()); ++i)
        manager_.addTurbine(i + 1, turbines[i].host, turbines[i].port, turbines[i].logicalDevice, turbines[i].iedName);

    // GOOSE Creation
    gooseReceiver = GooseReceiver_create();
    GooseReceiver_setInterfaceId(gooseReceiver, networkInterface.c_str());

    // Subscribe to GOOSE messages for each turbine and reference
    startGooseSubscription(1, IEC_STRINGS::GOOSE_SUB_TurSt, [](const std::string& ref, void* val) {
        (void)ref;
        (void)val;
        LIBIEC_LOG_V2("GOOSE callback for ref: " << ref << " enum=" << *((int32_t *)val));
    });

    
    LIBIEC_LOG_V1("registered " << turbines.size() << " turbine(s)");
    return IEC_OK;
}

// ── start / stop ─────────────────────────────────────────────────────────

void libiec_wrapper::start() { 
    // manager_.connectAll(); 
    manager_.connectTurbine(1);

    // GooseReceiver_start(gooseReceiver);
    // if (!GooseReceiver_isRunning(gooseReceiver)) {
    //     std::cerr << "[libiec_wrapper] Failed to start GooseReceiver\n";
    //     stop();
    // } else {
    //     std::cout << "[libiec_wrapper] GooseReceiver started successfully\n";
    // }
}
void libiec_wrapper::stop()  { 
    GooseReceiver_stop(gooseReceiver);

    GooseReceiver_destroy(gooseReceiver);
    manager_.disconnectAll(); 
}

IECReturnCode libiec_wrapper::startGooseSubscription(int turbineId, const std::string& daReference, GooseCallback callback) {
    // Build the full GOOSE reference: IEDName/LDName$LN$FC$GoCbName
    std::string fullRef = manager_.buildGooseRef(turbineId, daReference);
    char * daRef = const_cast<char *>(fullRef.c_str());
    auto subscriber = GooseSubscriber_create(daRef, NULL);

    // Set strict filters: multicast destination MAC + AppID
    // GOOSE multicast MAC per IEC 61850 standard
    uint8_t gooseMac[6] = {0x01, 0x0c, 0xcd, 0x01, 0x00, 0x01};
    GooseSubscriber_setDstMac(subscriber, gooseMac);
    GooseSubscriber_setAppId(subscriber, 1000);

    // Bridge C callback (function pointer) to C++ callback (std::function)
    // using a context struct passed through userData
    struct CallbackContext {
        GooseCallback userCallback;
    };

    auto* context = new CallbackContext{callback};
    
    GooseSubscriber_setListener(subscriber, [](GooseSubscriber subscriber, void* userData) {
        if (!userData) return;
        
        auto* ctx = reinterpret_cast<CallbackContext*>(userData);
        if (!ctx->userCallback) return;
        
        // printf("GOOSE event:\n");
        // printf("  stNum: %u sqNum: %u\n", GooseSubscriber_getStNum(subscriber),
        //         GooseSubscriber_getSqNum(subscriber));
        // printf("  timeToLive: %u\n", GooseSubscriber_getTimeAllowedToLive(subscriber));

        // uint64_t timestamp = GooseSubscriber_getTimestamp(subscriber);

        // printf("  timestamp: %u.%u\n", (uint32_t) (timestamp / 1000), (uint32_t) (timestamp % 1000));
        // printf("  message is %s\n", GooseSubscriber_isValid(subscriber) ? "valid" : "INVALID");

        MmsValue* values = GooseSubscriber_getDataSetValues(subscriber);
        
        // Get the reference from the GOOSE message itself
        const char* goCbRef = GooseSubscriber_getGoCbRef(subscriber);
        std::string refStr = goCbRef ? std::string(goCbRef) : "";

        // Decode first GOOSE dataset element as IEC enum (int32-backed).
        // IECEnumValue enumValue = IECEnumValue::Unknown;
        if (values) {
            MmsValue* enumField = values;

            MmsType dataSetType = MmsValue_getType(values);
            if (dataSetType == MMS_ARRAY || dataSetType == MMS_STRUCTURE)
                enumField = MmsValue_getElement(values, 0);

            if (enumField) {
                MmsType enumType = MmsValue_getType(enumField);
                if (enumType == MMS_INTEGER || enumType == MMS_UNSIGNED) {
                    // Invoke the C++ callback with decoded enum value.
                    int32_t intValue = MmsValue_toInt32(enumField);
                    ctx->userCallback(refStr, (void *) &intValue);
                }
            }
        }

    }, context);

    GooseReceiver_addSubscriber(gooseReceiver, subscriber);

    LIBIEC_LOG_V1("Started GOOSE subscription for turbine " << turbineId << ", ref: " << fullRef);

    return IEC_OK;
}

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

IECReturnCode libiec_wrapper::rxPowerGen(int turbineId, float& outPowerGen) {
    auto pw = manager_.readFloat(turbineId, manager_.buildRef(turbineId, IEC_STRINGS::POWER_MEAS), IEC61850_FC_MX);
    if (pw) {
        outPowerGen = *pw;
        return IEC_OK;
    }
    return IEC_ERROR;
}

IECReturnCode libiec_wrapper::txPowerSetpoint(int turbineId, float powerSetpoint) {
    bool ok = manager_.writeControlledFloat(turbineId, manager_.buildRef(turbineId, IEC_STRINGS::WTUR_DmdWSpt), powerSetpoint, false);
    return ok ? IEC_OK : IEC_ERROR;
}

IECReturnCode libiec_wrapper::txYawSetpoint(int turbineId, float yawSetpoint) {
    bool ok = manager_.writeControlledFloat(turbineId, manager_.buildRef(turbineId, IEC_STRINGS::XWYAW_YawSpt), yawSetpoint, false);
    return ok ? IEC_OK : IEC_ERROR;
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
