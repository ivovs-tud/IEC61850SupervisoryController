/*
 * Example usage:
 *
 *   IEC61850Manager mgr;
 *   mgr.addTurbine("WTG_01", "192.168.1.10", 102);
 *   mgr.addTurbine("WTG_02", "192.168.1.11", 102);
 *   mgr.connectAll();
 *
 *   // Inside a PeriodicTask at 10 Hz:
 *   auto voltage = mgr.readFloat("WTG_01",
 *                                "WTG01/MMXU1.PhV.phsA.cVal.mag.f",
 *                                IEC61850_FC_MX);
 *   if (voltage)
 *       std::cout << "Voltage: " << *voltage << "\n";
 *
 *   mgr.disconnectAll();
 */

#include "IEC61850Manager.hpp"
#include "common/config.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <thread>
#include <vector>

extern "C" {
#include <iec61850_client.h>
}

namespace {

std::string stripFcSuffix(const std::string& name)
{
    std::size_t pos = name.find(" [");
    if (pos == std::string::npos)
        return name;
    return name.substr(0, pos);
}

void collectDataAttributesRecursive(IedConnection connection,
                                    const std::string& objectReference,
                                    std::vector<std::string>& output,
                                    int depth = 0)
{
    if (depth > 32) {
        output.push_back(objectReference);
        return;
    }

    IedClientError err = IED_ERROR_OK;
    LinkedList children = IedConnection_getDataDirectoryFC(connection, &err, objectReference.c_str());

    if ((err != IED_ERROR_OK) || (children == NULL)) {
        output.push_back(objectReference);
        return;
    }

    bool hasChildren = false;
    LinkedList element = children;
    while ((element = LinkedList_getNext(element)) != NULL) {
        const char* childNameRaw = static_cast<const char*>(LinkedList_getData(element));
        if (childNameRaw == NULL)
            continue;

        std::string childName = stripFcSuffix(childNameRaw);
        if (childName.empty())
            continue;

        hasChildren = true;
        collectDataAttributesRecursive(connection, objectReference + "." + childName, output, depth + 1);
    }

    LinkedList_destroy(children);

    if (!hasChildren)
        output.push_back(objectReference);
}

} // anonymous namespace

// ── Logging helper aliases ────────────────────────────────────────────────
#define IEC_LOG(id, msg)  IECMGR_LOG_V1(id, msg)
#define IEC_DEBUG(id, msg) IECMGR_LOG_V2(id, msg)
#define IEC_ERR(id, msg)  IECMGR_ERR(id, msg)

// ── Forward declarations for select-and-operate ──────────────────────────
bool IEC61850Manager::performSelectAndOperate(
    void* controlObjectClient,
    void* mmsValue,
    int turbineId,
    const std::string& controlObjectReference,
    const std::string& functionName,
    bool useSelectBeforeOperate)
{
    auto* control = static_cast<ControlObjectClient>(controlObjectClient);
    auto* ctlVal = static_cast<MmsValue*>(mmsValue);

    if (useSelectBeforeOperate) {
        ControlModel controlModel = ControlObjectClient_getControlModel(control);
        bool selected = false;

        if (controlModel == CONTROL_MODEL_SBO_ENHANCED)
            selected = ControlObjectClient_selectWithValue(control, ctlVal);
        else
            selected = ControlObjectClient_select(control);

        if (!selected) {
            IEC_ERR(turbineId, functionName << "() – select failed for " << controlObjectReference
                               << " (err=" << ControlObjectClient_getLastError(control) << ")");
            return false;
        }
    }

    bool ok = ControlObjectClient_operate(control, ctlVal, 0);
    if (!ok) {
        IEC_ERR(turbineId, functionName << "() – operate failed for " << controlObjectReference
                           << " (err=" << ControlObjectClient_getLastError(control) << ")");
    }
    return ok;
}

bool IEC61850Manager::writeControlledGeneric(
    int turbineId,
    const std::string& controlObjectReference,
    const std::string& functionName,
    std::function<void*()> createMmsValue,
    bool useSelectBeforeOperate)
{
    std::lock_guard<std::mutex> mapLock(mapMutex_);
    auto it = turbines_.find(turbineId);
    if (it == turbines_.end()) {
        IEC_ERR(turbineId, functionName << "() – turbine not registered");
        return false;
    }

    TurbineConnection& tc = it->second;
    std::lock_guard<std::mutex> tcLock(tc.mutex);

    if (!tc.intentConnected)
        return false;

    if (!ensureConnected(tc)) {
        IEC_ERR(turbineId, functionName << "() – not connected, skipping control of " << controlObjectReference);
        return false;
    }

    ControlObjectClient control = ControlObjectClient_create(controlObjectReference.c_str(), tc.connection);
    if (!control) {
        IEC_ERR(turbineId, functionName << "() – failed to create control object " << controlObjectReference);
        return false;
    }

    MmsValue* ctlVal = static_cast<MmsValue*>(createMmsValue());
    bool ok = performSelectAndOperate(control, ctlVal, turbineId, controlObjectReference, functionName, useSelectBeforeOperate);
    MmsValue_delete(ctlVal);
    ControlObjectClient_destroy(control);

    return ok;
}

// ── Reconnect policy ─────────────────────────────────────────────────────
static constexpr int    MAX_RETRIES        = 5;
static constexpr int    BACKOFF_BASE_MS    = 100;  // doubled each retry

// ── Destructor ────────────────────────────────────────────────────────────

IEC61850Manager::~IEC61850Manager()
{
    disconnectAll();
}

// ── Registration ──────────────────────────────────────────────────────────

void IEC61850Manager::addTurbine(int id, const std::string& ip, int port) {
    addTurbine(id, ip, port, "", "");
}

void IEC61850Manager::addTurbine(int id, const std::string& ip, int port, const std::string& logicalDevice, const std::string& iedName) {
    std::lock_guard<std::mutex> mapLock(mapMutex_);
    if (turbines_.count(id)) {
        IEC_ERR(id, "already registered – ignoring duplicate addTurbine()");
        return;
    }
    // Use try_emplace so TurbineConnection is default-constructed in-place;
    // std::mutex is not moveable so we cannot construct it externally and move it.
    auto [it, inserted] = turbines_.try_emplace(id);
    if (!inserted) {
        IEC_ERR(id, "already registered – ignoring duplicate addTurbine()");
        return;
    }
    it->second.id     = id;
    it->second.ip     = ip;
    it->second.port   = port;
    it->second.logicalDevice = logicalDevice;
    it->second.iedName = iedName;
    it->second.status = IEC_LINK_CLOSED;
    IEC_LOG(id, "registered at " << ip << ":" << port);
}

std::string IEC61850Manager::buildRef(int turbineId, const std::string& daReference)
{
    if (daReference.empty())
        return daReference;

    std::lock_guard<std::mutex> mapLock(mapMutex_);
    auto it = turbines_.find(turbineId);
    if (it == turbines_.end())
        return daReference;

    const TurbineConnection& tc = it->second;

    // Already absolute in common formats (IED/..., LD/...)
    if (daReference.find('/') != std::string::npos)
        return daReference;

    if (tc.logicalDevice.empty())
        return daReference;

    if (tc.iedName.empty())
        return tc.logicalDevice + "." + daReference;

    return tc.iedName + tc.logicalDevice + "/" + daReference;
}

std::string IEC61850Manager::buildGooseRef(int turbineId, const std::string& goCbRef)
{
    if (goCbRef.empty())
        return goCbRef;

    // Already absolute (contains '/')
    if (goCbRef.find('/') != std::string::npos)
        return goCbRef;

    std::lock_guard<std::mutex> mapLock(mapMutex_);
    auto it = turbines_.find(turbineId);
    if (it == turbines_.end())
        return goCbRef;

    const TurbineConnection& tc = it->second;

    if (tc.logicalDevice.empty())
        return goCbRef;

    // GOOSE format: IEDName/LDName$LN$FC$GoCbName
    if (tc.iedName.empty())
        return tc.logicalDevice + "$" + goCbRef;

    return tc.iedName + "" + tc.logicalDevice + "/" + goCbRef;
}

// ── Internal helpers ──────────────────────────────────────────────────────

bool IEC61850Manager::doConnect(TurbineConnection& tc)
{
    // Clean up any stale handle first.
    if (tc.connection) {
        IedConnection_destroy(tc.connection);
        tc.connection = nullptr;
    }

    IedClientError err;
    tc.status     = IEC_LINK_CONNECTING;
    tc.connection = IedConnection_create();
    IedConnection_connect(tc.connection, &err, tc.ip.c_str(), tc.port);

    if (err != IED_ERROR_OK) {
        IEC_ERR(tc.id, "connect failed (error " << err << ")");
        IedConnection_destroy(tc.connection);
        tc.connection = nullptr;
        tc.status     = IEC_LINK_ERROR;
        return false;
    }

    tc.status           = IEC_LINK_CONNECTED;
    tc.intentConnected  = true;
    IEC_LOG(tc.id, "connected to " << tc.ip << ":" << tc.port);
    return true;
}

void IEC61850Manager::doDisconnect(TurbineConnection& tc)
{
    if (tc.connection) {
        IedConnection_close(tc.connection);
        IedConnection_destroy(tc.connection);
        tc.connection = nullptr;
    }
    tc.status          = IEC_LINK_CLOSED;
    tc.intentConnected = false;
    IEC_LOG(tc.id, "disconnected");
}

bool IEC61850Manager::ensureConnected(TurbineConnection& tc)
{
    // Only reconnect if the caller previously established an intentional
    // connection via connectTurbine() or connectAll().  If the turbine was
    // never explicitly connected (or was manually disconnected) we leave it
    // alone so callers get a clear nullopt / false rather than a surprise
    // blocking reconnect attempt.
    if (!tc.intentConnected)
        return false;

    // Already in a good state?
    if (tc.connection &&
        IedConnection_getState(tc.connection) == IED_STATE_CONNECTED)
    {
        return true;
    }

    // Attempt reconnect with exponential backoff.
    tc.status = IEC_LINK_RECONNECTING;
    int delayMs = BACKOFF_BASE_MS;
    for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
        IEC_LOG(tc.id, "reconnect attempt " << attempt << "/" << MAX_RETRIES
                    << " (backoff " << delayMs << " ms)");
        if (doConnect(tc))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        delayMs *= 2;
    }

    IEC_ERR(tc.id, "all reconnect attempts exhausted – giving up");
    tc.status = IEC_LINK_ERROR;
    return false;
}

// ── Public connection management ─────────────────────────────────────────

bool IEC61850Manager::connectTurbine(int id)
{
    std::lock_guard<std::mutex> mapLock(mapMutex_);
    auto it = turbines_.find(id);
    if (it == turbines_.end()) {
        IEC_ERR(id, "connectTurbine() – turbine not registered");
        return false;
    }
    TurbineConnection& tc = it->second;
    std::lock_guard<std::mutex> tcLock(tc.mutex);
    // Mark this turbine as intentionally connected even if the first attempt
    // fails, so subsequent ensureConnected() calls keep retrying.
    tc.intentConnected = true;
    return doConnect(tc);
}

void IEC61850Manager::disconnectTurbine(int id)
{
    std::lock_guard<std::mutex> mapLock(mapMutex_);
    auto it = turbines_.find(id);
    if (it == turbines_.end()) {
        IEC_ERR(id, "disconnectTurbine() – turbine not registered");
        return;
    }
    TurbineConnection& tc = it->second;
    std::lock_guard<std::mutex> tcLock(tc.mutex);
    doDisconnect(tc);
}

void IEC61850Manager::connectAll()
{
    // Snapshot the keys so we don't hold mapMutex_ while connecting
    // (connecting can take time and connectTurbine() re-acquires mapMutex_).
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> mapLock(mapMutex_);
        for (auto& [id, _] : turbines_)
            ids.push_back(id);
    }
    for (int id : ids)
        connectTurbine(id);
}

void IEC61850Manager::disconnectAll()
{
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> mapLock(mapMutex_);
        for (auto& [id, _] : turbines_)
            ids.push_back(id);
    }
    for (int id : ids)
        disconnectTurbine(id);
}

// ── Read / Write ──────────────────────────────────────────────────────────

std::optional<float> IEC61850Manager::readFloat(int turbineId,
                                                  const std::string& daReference,
                                                  int fc)
{
    std::lock_guard<std::mutex> mapLock(mapMutex_);
    auto it = turbines_.find(turbineId);
    if (it == turbines_.end()) {
        IEC_ERR(turbineId, "readFloat() – turbine not registered");
        return std::nullopt;
    }
    TurbineConnection& tc = it->second;
    std::lock_guard<std::mutex> tcLock(tc.mutex);

    if (!tc.intentConnected)
        return std::nullopt;

    if (!ensureConnected(tc)) {
        IEC_ERR(turbineId, "readFloat() – not connected, skipping read of " << daReference);
        return std::nullopt;
    }

    IedClientError err;
    MmsValue* mmsVal = IedConnection_readObject(
        tc.connection, &err,
        daReference.c_str(),
        static_cast<FunctionalConstraint>(fc));

    if (err != IED_ERROR_OK || !mmsVal) {
        IEC_ERR(turbineId, "readFloat() failed for " << daReference
                    << " (error " << err << ")");
        return std::nullopt;
    }

    float result = MmsValue_toFloat(mmsVal);
    MmsValue_delete(mmsVal);
    return result;
}

bool IEC61850Manager::writeFloat(int turbineId,
                                  const std::string& daReference,
                                  int fc,
                                  float value)
{
    std::lock_guard<std::mutex> mapLock(mapMutex_);
    auto it = turbines_.find(turbineId);
    if (it == turbines_.end()) {
        IEC_ERR(turbineId, "writeFloat() – turbine not registered");
        return false;
    }
    TurbineConnection& tc = it->second;
    std::lock_guard<std::mutex> tcLock(tc.mutex);

    if (!tc.intentConnected)
        return false;

    if (!ensureConnected(tc)) {
        IEC_ERR(turbineId, "writeFloat() – not connected, skipping write of " << daReference);
        return false;
    }

    MmsValue* mmsVal = MmsValue_newFloat(value);
    IedClientError err;
    IedConnection_writeObject(
        tc.connection, &err,
        daReference.c_str(),
        static_cast<FunctionalConstraint>(fc),
        mmsVal);
    MmsValue_delete(mmsVal);

    if (err != IED_ERROR_OK) {
        IEC_ERR(turbineId, "writeFloat() failed for " << daReference
                    << " = " << value << " (error " << err << ")");
        return false;
    }
    return true;
}

bool IEC61850Manager::writeControlledFloat(int turbineId,
                                           const std::string& controlObjectReference,
                                           float value,
                                           bool useSelectBeforeOperate)
{
    return writeControlledGeneric(turbineId, controlObjectReference, "writeControlledFloat",
                                  [value]() { return static_cast<void*>(MmsValue_newFloat(value)); },
                                  useSelectBeforeOperate);
}

bool IEC61850Manager::writeControlledInt(int turbineId,
                                         const std::string& controlObjectReference,
                                         int value,
                                         bool useSelectBeforeOperate)
{
    return writeControlledGeneric(turbineId, controlObjectReference, "writeControlledInt",
                                  [value]() { return static_cast<void*>(MmsValue_newIntegerFromInt32(value)); },
                                  useSelectBeforeOperate);
}

bool IEC61850Manager::writeControlledEnum(int turbineId,
                                          const std::string& controlObjectReference,
                                          int enumOrdinal,
                                          bool useSelectBeforeOperate)
{
    // MMS enumerated values are encoded as integers on the wire.
    return writeControlledGeneric(turbineId, controlObjectReference, "writeControlledEnum",
                                  [enumOrdinal]() { return static_cast<void*>(MmsValue_newIntegerFromInt32(enumOrdinal)); },
                                  useSelectBeforeOperate);
}

std::optional<int> IEC61850Manager::readInt(int turbineId,
                                              const std::string& daReference,
                                              int fc)
{
    std::lock_guard<std::mutex> mapLock(mapMutex_);
    auto it = turbines_.find(turbineId);
    if (it == turbines_.end()) {
        IEC_ERR(turbineId, "readInt() – turbine not registered");
        return std::nullopt;
    }
    TurbineConnection& tc = it->second;
    std::lock_guard<std::mutex> tcLock(tc.mutex);

    if (!tc.intentConnected)
        return std::nullopt;

    if (!ensureConnected(tc)) {
        IEC_ERR(turbineId, "readInt() – not connected, skipping read of " << daReference);
        return std::nullopt;
    }

    IedClientError err;
    MmsValue* mmsVal = IedConnection_readObject(
        tc.connection, &err,
        daReference.c_str(),
        static_cast<FunctionalConstraint>(fc));

    if (err != IED_ERROR_OK || !mmsVal) {
        IEC_ERR(turbineId, "readInt() failed for " << daReference
                    << " (error " << err << ")");
        return std::nullopt;
    }

    int result = MmsValue_toInt32(mmsVal);
    MmsValue_delete(mmsVal);
    return result;
}

bool IEC61850Manager::writeInt(int turbineId,
                                const std::string& daReference,
                                int fc,
                                int value)
{
    std::lock_guard<std::mutex> mapLock(mapMutex_);
    auto it = turbines_.find(turbineId);
    if (it == turbines_.end()) {
        IEC_ERR(turbineId, "writeInt() – turbine not registered");
        return false;
    }
    TurbineConnection& tc = it->second;
    std::lock_guard<std::mutex> tcLock(tc.mutex);

    if (!tc.intentConnected)
        return false;

    if (!ensureConnected(tc)) {
        IEC_ERR(turbineId, "writeInt() – not connected, skipping write of " << daReference);
        return false;
    }

    MmsValue* mmsVal = MmsValue_newIntegerFromInt32(value);
    IedClientError err;
    IedConnection_writeObject(
        tc.connection, &err,
        daReference.c_str(),
        static_cast<FunctionalConstraint>(fc),
        mmsVal);
    MmsValue_delete(mmsVal);

    if (err != IED_ERROR_OK) {
        IEC_ERR(turbineId, "writeInt() failed for " << daReference
                    << " = " << value << " (error " << err << ")");
        return false;
    }
    return true;
}

std::optional<std::string> IEC61850Manager::readString(int turbineId,
                                                        const std::string& daReference,
                                                        int fc)
{
    std::lock_guard<std::mutex> mapLock(mapMutex_);
    auto it = turbines_.find(turbineId);
    if (it == turbines_.end()) {
        IEC_ERR(turbineId, "readString() – turbine not registered");
        return std::nullopt;
    }

    TurbineConnection& tc = it->second;
    std::lock_guard<std::mutex> tcLock(tc.mutex);

    if (!tc.intentConnected)
        return std::nullopt;

    if (!ensureConnected(tc)) {
        IEC_ERR(turbineId, "readString() – not connected, skipping read of " << daReference);
        return std::nullopt;
    }

    IedClientError err;
    MmsValue* mmsVal = IedConnection_readObject(
        tc.connection, &err,
        daReference.c_str(),
        static_cast<FunctionalConstraint>(fc));

    if (err != IED_ERROR_OK || !mmsVal) {
        IEC_ERR(turbineId, "readString() failed for " << daReference
                    << " (error " << err << ")");
        return std::nullopt;
    }

    const char* text = MmsValue_toString(mmsVal);
    std::string result = text ? std::string(text) : std::string();
    MmsValue_delete(mmsVal);

    return result;
}

// ── Model interrogation ─────────────────────────────────────────────────────────

std::map<std::string, bool> IEC61850Manager::checkSupported(
    int turbineId,
    const std::vector<std::string>& references,
    int fc)
{
    std::map<std::string, bool> result;

    std::lock_guard<std::mutex> mapLock(mapMutex_);
    auto it = turbines_.find(turbineId);
    if (it == turbines_.end()) {
        IEC_ERR(turbineId, "checkSupported() – turbine not registered");
        return result;
    }
    TurbineConnection& tc = it->second;
    std::lock_guard<std::mutex> tcLock(tc.mutex);

    if (!tc.intentConnected) {
        IEC_ERR(turbineId, "checkSupported() – turbine not intentionally connected");
        return result;
    }

    if (!ensureConnected(tc)) {
        IEC_ERR(turbineId, "checkSupported() – not connected");
        return result;
    }

    for (const auto& ref : references) {
        IedClientError err;
        MmsValue* val = IedConnection_readObject(
            tc.connection, &err,
            ref.c_str(),
            static_cast<FunctionalConstraint>(fc));

        bool supported = false;
        if (val) {
            // Successfully read – object definitely exists.
            MmsValue_delete(val);
            supported = true;
        } else if (err == IED_ERROR_ACCESS_DENIED ||
                   err == IED_ERROR_OBJECT_ACCESS_UNSUPPORTED) {
            // Server rejected the read but the object is present on the IED.
            // ACCESS_DENIED      – the FC/reference is valid but restricted.
            // OBJECT_ACCESS_UNSUPPORTED – wrong FC for this DA, object still real.
            supported = true;
        }
        // Everything else (OBJECT_DOES_NOT_EXIST, OBJECT_REFERENCE_INVALID,
        // OBJECT_UNDEFINED, SERVICE_NOT_SUPPORTED, TIMEOUT, UNKNOWN, …)
        // means the reference is absent or the server cannot serve it,
        // so supported stays false.

        result[ref] = supported;
        IEC_LOG(turbineId, "checkSupported " << ref
                    << " → " << (supported ? "supported" : "NOT supported")
                    << " (err=" << err << ")");
    }
    return result;
}

std::vector<std::string> IEC61850Manager::getDataModelReferences(int turbineId)
{
    std::vector<std::string> refs;

    std::lock_guard<std::mutex> mapLock(mapMutex_);
    auto it = turbines_.find(turbineId);
    if (it == turbines_.end()) {
        IEC_ERR(turbineId, "getDataModelReferences() – turbine not registered");
        return refs;
    }

    TurbineConnection& tc = it->second;
    std::lock_guard<std::mutex> tcLock(tc.mutex);

    if (!tc.intentConnected) {
        IEC_ERR(turbineId, "getDataModelReferences() – turbine not intentionally connected");
        return refs;
    }

    if (!ensureConnected(tc)) {
        IEC_ERR(turbineId, "getDataModelReferences() – not connected");
        return refs;
    }

    IedClientError err = IED_ERROR_OK;
    LinkedList ldList = IedConnection_getServerDirectory(tc.connection, &err, false);
    if ((err != IED_ERROR_OK) || (ldList == NULL)) {
        IEC_ERR(turbineId, "getDataModelReferences() – failed to get logical devices (err=" << err << ")");
        return refs;
    }

    LinkedList ldElem = ldList;
    while ((ldElem = LinkedList_getNext(ldElem)) != NULL) {
        const char* ldName = static_cast<const char*>(LinkedList_getData(ldElem));
        if (ldName == NULL)
            continue;

        LinkedList lnList = IedConnection_getLogicalDeviceDirectory(tc.connection, &err, ldName);
        if ((err != IED_ERROR_OK) || (lnList == NULL))
            continue;

        LinkedList lnElem = lnList;
        while ((lnElem = LinkedList_getNext(lnElem)) != NULL) {
            const char* lnName = static_cast<const char*>(LinkedList_getData(lnElem));
            if (lnName == NULL)
                continue;

            std::string lnRef = std::string(ldName) + "/" + lnName;
            LinkedList doList = IedConnection_getLogicalNodeDirectory(
                tc.connection, &err, lnRef.c_str(), ACSI_CLASS_DATA_OBJECT);

            if ((err != IED_ERROR_OK) || (doList == NULL))
                continue;

            LinkedList doElem = doList;
            while ((doElem = LinkedList_getNext(doElem)) != NULL) {
                const char* doName = static_cast<const char*>(LinkedList_getData(doElem));
                if (doName == NULL)
                    continue;

                collectDataAttributesRecursive(tc.connection, lnRef + "." + doName, refs);
            }

            LinkedList_destroy(doList);
        }

        LinkedList_destroy(lnList);
    }

    LinkedList_destroy(ldList);

    std::sort(refs.begin(), refs.end());
    refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
    return refs;
}

void IEC61850Manager::printDataModel(int turbineId, int maxEntries)
{
    std::vector<std::string> refs = getDataModelReferences(turbineId);

    if (refs.empty()) {
        IEC_DEBUG(turbineId, "printDataModel() – no entries found");
        return;
    }

    std::size_t limit = refs.size();
    if (maxEntries > 0)
        limit = std::min<std::size_t>(limit, static_cast<std::size_t>(maxEntries));

    IEC_DEBUG(turbineId, "Data model entries: showing " << limit << " of " << refs.size());
    for (std::size_t i = 0; i < limit; ++i)
        IEC_DEBUG(turbineId, "  [" << i << "] " << refs[i]);
}
