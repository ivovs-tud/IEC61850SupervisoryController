#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Forward-declare the libiec61850 connection handle so this header stays
// clean of C includes.  The actual libiec61850 headers are pulled in by the
// .cpp with an extern "C" guard.
struct sIedConnection;
typedef struct sIedConnection* IedConnection;

// Functional Constraint is passed as `int` in the public API so that this
// header does not need to include the libiec61850 C headers.  Callers should
// use the IEC61850_FC_* integer constants defined in <iec61850_client.h>.
// Example: mgr.readFloat(id, ref, IEC61850_FC_MX);

// Connection lifecycle status for a single IEC 61850 turbine link.
typedef enum {
    IEC_LINK_CLOSED       =  0,
    IEC_LINK_CONNECTING   =  1,
    IEC_LINK_CONNECTED    =  2,
    IEC_LINK_RECONNECTING =  3,
    IEC_LINK_ERROR        = -1,
} IecConnectionStatus;

// ---------------------------------------------------------------------------
// TurbineConnection – per-turbine state held inside IEC61850Manager.
// Turbine IDs are integers: 0 = supercontroller (this process),
// 1…N = individual wind turbines.
// ---------------------------------------------------------------------------
struct TurbineConnection
{
    int                 id               {0};
    uint8_t             mac[6]           {0x00, 0x15, 0x5d, 0xb4, 0x81, 0xad};  ///< Populated from IED on connect, used for GOOSE subscription filtering
    uint16_t            appId            {1000};       ///< Optional GOOSE AppID for subscription filtering
    std::string         ip;
    int                 port             {102};
    std::string         logicalDevice;
    std::string         iedName;
    IedConnection       connection       {nullptr};  ///< nullptr = not connected
    std::mutex          mutex;                       ///< per-turbine lock
    IecConnectionStatus status           {IEC_LINK_CLOSED};
    bool                intentConnected  {false};    ///< true only after an explicit connect call
};

// ---------------------------------------------------------------------------
/// @class IEC61850Manager
/// @brief Manages persistent MMS connections to multiple wind turbines over
///        IEC 61850, using the libiec61850 C library.
///
/// Typical usage:
/// @code
///   IEC61850Manager mgr;
///   mgr.addTurbine("WTG_01", "192.168.1.10", 102);
///   mgr.connectAll();
///
///   // Inside a PeriodicTask:
///   auto v = mgr.readFloat("WTG_01", "WTG01/MMXU1.PhV.phsA.cVal.mag.f",
///                           IEC61850_FC_MX);
///   if (v) std::cout << "Voltage: " << *v << "\n";
/// @endcode
// ---------------------------------------------------------------------------
class IEC61850Manager
{
public:
    IEC61850Manager()  = default;
    ~IEC61850Manager();
    /** @brief Calls disconnectAll() on destruction. */

    IEC61850Manager(const IEC61850Manager&)            = delete;
    IEC61850Manager& operator=(const IEC61850Manager&) = delete;

    // ── Registration ────────────────────────────────────────────────────────

    void addTurbine(int id, const std::string& ip, int port = 102);
    /**
     * @brief Register a turbine without connecting yet.
     * @param id   Integer turbine ID >= 1 (ID 0 is reserved for the supercontroller).
     * @param ip   IP address of the turbine MMS server.
     * @param port MMS port (default 102).
     */

    void addTurbine(int id,
                    const std::string& ip,
                    int port,
                    const std::string& logicalDevice,
                    const std::string& iedName = "");
    /**
     * @brief Register a turbine with logical-device metadata for MMS reference generation.
     * @param logicalDevice Logical Device name (e.g. "WTGLD1").
     * @param iedName Optional IED name prefix. If empty, buildRef() returns LD/LN.DO.DA.
     */

    // ── Connection management ────────────────────────────────────────────────

    bool connectTurbine(int id);
    /**
     * @brief Establish a connection to a single turbine.
     * @return true on success.
     */

    void disconnectTurbine(int id);
    /** @brief Disconnect a single turbine. */

    void connectAll();
    /** @brief Connect to all registered turbines. */

    void disconnectAll();
    /** @brief Disconnect from all registered turbines. */

    // ── Read / Write ────────────────────────────────────────────────────────

    std::optional<float> readFloat(int turbineId,
                                   const std::string& daReference,
                                   int fc);
    /**
     * @brief Read a float value from a turbine data attribute.
     * @param turbineId   Integer turbine ID (1-based).
     * @param daReference IEC 61850 DA reference.
     * @param fc          Functional Constraint integer (IEC61850_FC_MX, etc.).
     * @return The float value, or std::nullopt on failure.
     */

    bool writeFloat(int turbineId,
                    const std::string& daReference,
                    int fc,
                    float value);
    /**
     * @brief Write a float value to a turbine data attribute.
     * @return true on success, false on failure.
     */

    bool writeControlledFloat(int turbineId,
                              const std::string& controlObjectReference,
                              float value,
                              bool useSelectBeforeOperate);

    bool writeControlledInt(int turbineId,
                            const std::string& controlObjectReference,
                            int value,
                            bool useSelectBeforeOperate);

    bool writeControlledEnum(int turbineId,
                             const std::string& controlObjectReference,
                             int enumOrdinal,
                             bool useSelectBeforeOperate);
    /**
     * @brief Write a control value using IEC 61850 control services.
     *
     * Uses ControlObjectClient with either direct operate or
     * select-before-operate (SBO) followed by operate.
     *
     * For SBO, the method chooses select()/selectWithValue() based on the
     * server control model reported by the control object.
     *
     * @param controlObjectReference IEC 61850 control object reference.
     * @param value Float/integer control value; enumOrdinal for enum variant.
     * @param useSelectBeforeOperate true to perform SBO, false for direct operate.
     * @return true on success, false on failure.
     */

    std::optional<int> readInt(int turbineId,
                               const std::string& daReference,
                               int fc);
    /**
     * @brief Read an integer value from a turbine data attribute.
     * @return The integer value, or std::nullopt on failure.
     */

    bool writeInt(int turbineId,
                  const std::string& daReference,
                  int fc,
                  int value);
    /**
     * @brief Write an integer value to a turbine data attribute.
     * @return true on success, false on failure.
     */

    std::optional<std::string> readString(int turbineId,
                                          const std::string& daReference,
                                          int fc);
    /** @brief Read a string from a turbine data attribute. */

    std::string buildRef(int turbineId, const std::string& daReference);
    /**
     * @brief Build a full MMS reference from per-turbine IED/LD metadata.
     *
     * If daReference already starts with "IED/" or "LD/" it is returned unchanged.
     * Otherwise this prepends "IED/LD/" (when IED is configured) or "LD/".
     */

    std::string buildGooseRef(int turbineId, const std::string& goCbRef);
    /**
     * @brief Build a full GOOSE control block reference from per-turbine IED/LD metadata.
     *
     * GOOSE references use a different format from MMS:
     *   IEDName/LDName$LNName$GO$GoCbName
     * e.g. "WTURBINE/LD0$WTUR1$GO$TurSt"
     *
     * If goCbRef already contains '/' it is returned unchanged (already absolute).
     */

    // ── Model interrogation ──────────────────────────────────────────────────

    std::map<std::string, bool> checkSupported(int turbineId,
                                               const std::vector<std::string>& references,
                                               int fc);
    /**
     * @brief Check whether a list of DA references are readable on a turbine.
     *
     * Each reference is probed with a single read. A reference is reported
     * as supported if the server returns a value without an
     * OBJECT_REFERENCE_INVALID or OBJECT_DOES_NOT_EXIST error. Access-denied
     * responses are treated as "supported" (the object exists on the server).
     *
     * @param turbineId Integer turbine ID (1-based).
     * @param references List of DA references to probe.
     * @param fc Functional Constraint to use for the probe reads.
     * @return Map of reference -> supported (true/false).
     *         Returns an empty map if the turbine is not registered or not
     *         intentionally connected.
     */

    std::vector<std::string> getDataModelReferences(int turbineId);
    /**
     * @brief Retrieve a flattened list of full data attribute references available in the connected IED.
     *
     * References are returned in canonical server format (e.g. LD/LN.DO.DA...)
     * and deduplicated/sorted.
     */

    void printDataModel(int turbineId, int maxEntries = 200);
    /** @brief Print up to maxEntries from the flattened IED data model list. */

private:
    // ── Internal helpers ────────────────────────────────────────────────────

    bool ensureConnected(TurbineConnection& tc);
    /**
     * @brief Check connection state and reconnect with exponential backoff if
     *        necessary. Must be called with the turbine mutex already held.
     * @return true if the connection is ready after this call.
     */

    bool doConnect(TurbineConnection& tc);
    /**
     * @brief Attempt a single connection attempt for the given turbine.
     *        Caller must hold the turbine mutex.
     */

    void doDisconnect(TurbineConnection& tc);
    /**
     * @brief Close and nullify the IedConnection for the given turbine.
     *        Caller must hold the turbine mutex.
     */

    bool performSelectAndOperate(void* controlObjectClient,
                                 void* mmsValue,
                                 int turbineId,
                                 const std::string& controlObjectReference,
                                 const std::string& functionName,
                                 bool useSelectBeforeOperate);
    /**
     * @brief Common select-and-operate logic for controlled writes.
     *        Caller must hold the turbine mutex.  Passed as void* to avoid
     *        including libiec61850 headers in this file.
     * @return true if select+operate succeeded, false otherwise.
     */

    bool writeControlledGeneric(int turbineId,
                                const std::string& controlObjectReference,
                                const std::string& functionName,
                                std::function<void*()> createMmsValue,
                                bool useSelectBeforeOperate);
    /**
     * @brief Generic helper for all writeControlled* variants.
     *        Handles turbine lookup, connection check, control object creation,
     *        MmsValue cleanup, and error logging. Returns early if lookup/connection fails.
     * @param createMmsValue Callback that creates and returns an MmsValue* (void* for header purity).
     * @return true if operate succeeded, false otherwise.
     */

    // Map from turbine ID → connection state.
    // The map itself is protected by mapMutex_ for insertions; individual
    // turbine entries are protected by their own TurbineConnection::mutex.
    std::map<int, TurbineConnection> turbines_;
    std::mutex                       mapMutex_;
};
