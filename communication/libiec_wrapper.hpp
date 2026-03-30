#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>


#include "IEC61850Manager.hpp"

extern "C" {
    struct sGooseReceiver;
    typedef struct sGooseReceiver* GooseReceiver;

    struct sGooseSubscriber;
    typedef struct sGooseSubscriber* GooseSubscriber;
}


// ---------------------------------------------------------------------------
// libiec_wrapper – high-level C++ wrapper around IEC61850Manager.
// ---------------------------------------------------------------------------

typedef enum r 
{
    IEC_OK = 0,
    IEC_ERROR = -1,
} IECReturnCode;


using GooseCallback = std::function<void(const std::string&, void*)>;

// typedef GooseCallback (*GooseCallback)(std::string& ref, void* value);

namespace IEC_STRINGS {

// IEC 61850 DA references — LN.DO.DA paths WITHOUT the Logical Device (LD)
// prefix.  The LD is configured per turbine via TurbineEndpoint::logicalDevice
// and prepended automatically by libiec_wrapper using IEC61850Manager::buildRef().
static constexpr const char* WTUR_DmdWSpt = "WTUR1.DmdWSpt";   // APC – active-power setpoint
static constexpr const char* XWYAW_YawSpt = "WYAW1.YwAngSpt";    // APC – yaw-angle setpoint

static constexpr const char* WTUR_TurSt   = "WTUR1.TurSt";   // Wind Turbine State [ST]
static constexpr const char* WTUR_OP_CMD  = "WTUR1.TurOp";   // Wind Turbine Operation Command [CMD]

static constexpr const char* WROT_RotBlk  = "WROT1.RotBlk";  // Block Rotor Position Command [CMD]
// static constexpr const char* WROT_PthEmgChk = "WROT1.PthEmgChk";

/** DA references for reading operational data */
static constexpr const char* POWER_MEAS = "WTUR1.W.f";            // Measured active power
static constexpr const char* YAW_MEAS   = "WYAW1.YwAng.mag.f";  // Measured yaw angle
static constexpr const char* WS_MEAS    = "WMET1.HorWdSpd.mag.f";  // Measured wind speed
static constexpr const char* WD_MEAS    = "WMET1.HorWdDir.mag.f";  // Measured wind direction
static constexpr const char* RPM_MEAS   = "WROT1.RotSpd.mag.f";    // Measured rotor speed
static constexpr const char* TOT_W      = "WTUR1.TotWh.f";       // Measured total power

static constexpr const char* PITCH_SP   = "WROT1.BlPthAngTgt.f"; // Pitch Angle Target Value
static constexpr const char* PITCH_VAL  = "WROT1.BlPthAngVal.f"; // Pitch Angle Feedback

static constexpr const char* SECR_S     = "SECR1.S.stVal";        // Secret LN [ST]

static const std::vector<std::string> REQ_CMDS = {WTUR_DmdWSpt, XWYAW_YawSpt, WTUR_OP_CMD};
static const std::vector<std::string> REQ_REFS = {POWER_MEAS, YAW_MEAS, WS_MEAS, WD_MEAS, RPM_MEAS, TOT_W, PITCH_VAL, SECR_S};

static const std::vector<std::string> GOOSE_SUB_REFS = {
    "WTUR1$GO$TurSt",  // GOOSE with turbine state changes
    "WTUR1$GO$Alm",    // GOOSE with turbine alarms
};
};

// Per-turbine MMS connection parameters.
// Multiple turbines may share the same host+port (same physical IED) but use
// different Logical Devices, or each may have its own host and/or port.
struct TurbineEndpoint {
    std::string host;
    int         port          {102};       ///< MMS TCP port
    std::string iedName       {};          ///< Optional IED name prefix
    std::string logicalDevice {"WTGLD1"};  ///< IEC 61850 Logical Device name prefix

    // Goose Related
    // std::string networkIface  {"eth0"};    ///< Network interface for GOOSE. TODO: allow configuration
    uint8_t     mac[6]        {0};         ///< Optional MAC address for GOOSE subscription filtering
        
    GooseReceiver gooseReceiver {nullptr};
    std::vector<std::string> gooseRefs {}; ///< List of GOOSE DA references to subscribe to on this turbine
    std::vector<GooseSubscriber> gooseSubscribers {};
    std::vector<GooseCallback> gooseCallbacks {};
};

class libiec_wrapper
{
public:
    libiec_wrapper()  = default;
    ~libiec_wrapper() = default;

    IECReturnCode init(const std::vector<TurbineEndpoint>& turbines, std::string networkInterface = "eth0");
    /**
     * @brief Register turbines (IDs 1…N) and prepare MMS connections.
     * @param turbines  Per-turbine endpoint config; turbines[i] maps to turbine ID i+1.
     * @return IEC_OK on success, IEC_ERROR if the vector is empty.
     */

    void start();
    /** @brief Connect to all registered turbines. */

    void stop();
    /** @brief Disconnect from all registered turbines. */

    IECReturnCode startGooseSubscription(int turbineId, const std::string& daReference, GooseCallback callback);

    IECReturnCode txSetpoint(int turbineId, float powerSetpoint, int yawSetpoint);
    /**
     * @brief Transmit power and yaw setpoints to a specific turbine.
     * @param turbineId      Integer turbine ID (1-based).
     * @param powerSetpoint  Active-power setpoint in watts.
     * @param yawSetpoint    Yaw setpoint in degrees.
     * @return IEC_OK on success, IEC_ERROR if either write fails.
     */

    IECReturnCode rxSecret(int turbineId, std::string& outSecret);
    /**
     * @brief Read the secret value from a specific turbine.
     * @param turbineId  Integer turbine ID (1-based).
     * @param outSecret  Output string populated with the secret on success.
     * @return IEC_OK on success, IEC_ERROR on failure.
     */

    IECReturnCode rxWindSpeed(int turbineId, float& outWindSpeed);
    /**
     * @brief Read the current wind speed measurement from a specific turbine.
     * @param turbineId      Integer turbine ID (1-based).
     * @param outWindSpeed   Output float populated with the wind speed in m/s on success.
     * @return IEC_OK on success, IEC_ERROR on failure.
     */

    IECReturnCode rxWindDirection(int turbineId, float& outWindDirection);
    /**
     * @brief Read the current wind direction measurement from a specific turbine.
     * @param turbineId          Integer turbine ID (1-based).
     * @param outWindDirection   Output float populated with the wind direction in degrees from north on success.
     * @return IEC_OK on success, IEC_ERROR on failure.
     */

    IECReturnCode rxRotorSpeed(int turbineId, float& outRPM);
    /**
     * @brief Read the current rotor speed measurement from a specific turbine. 
     * @param turbineId  Integer turbine ID (1-based).
     * @param outRPM     Output float populated with the rotor speed in RPM on success.
     * @return IEC_OK on success, IEC_ERROR on failure.
     */

    std::map<std::string, bool> checkTurbineSupport(int turbineId, const std::vector<std::string>& references, int fc = 2 /* IEC61850_FC_SP */);
    /**
     * @brief Check whether a list of DA references are supported by a turbine.
     *
     * Delegates to IEC61850Manager::checkSupported — uses model browsing,
     * not blind reads.
     *
     * @param turbineId  Integer turbine ID (1-based).
     * @param references DA references to probe.
     * @param fc         Functional Constraint integer (IEC61850_FC_SP by default).
     * @return Map of reference → supported (true/false).
     */

    void printTurbineDataModel(int turbineId, int maxEntries = 50);

    /// @brief Retrieve flattened full IEC 61850 data model references from turbine's connected IED.
    std::vector<std::string> getTurbineDataModel(int turbineId);
    /**
     * @brief Print up to @p maxEntries DA references exposed by a turbine.
     *        Delegates to IEC61850Manager::printDataModel.
     * @param turbineId  Integer turbine ID (1-based).
     * @param maxEntries Maximum number of lines to print.
     */

private:
    IEC61850Manager manager_;
};

