#pragma once

#include "sc/communication/attack/AttackSignalType.hpp"
#include "sc/runtime/Clock.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sc::application {

using AttackSessionId = uint64_t;

struct AttackSessionInfo {
    AttackSessionId id{0};
    std::string label;
    sc::ports::Clock::SteadyTimePoint lastHeartbeat;
};

struct StartAttackSessionResult {
    AttackSessionId id{0};
    std::string error;

    explicit operator bool() const { return id != 0; }
    /**
     * Checks whether the session was started.
     * @return True when a session ID was assigned.
     */
};

struct ClosedAttackSession {
    AttackSessionId id{0};
    std::string label;
    std::string reason;
};

// Tracks the active attack client and its per-turbine controls.
class AttackSessionManager {
public:
    AttackSessionManager(int numTurbines, std::vector<AttackInterface::SignalType> signalTypes, sc::ports::Clock& clock, std::chrono::milliseconds leaseTimeout = std::chrono::milliseconds(750));
    /**
     * Creates attack state for all configured turbine links.
     * @param numTurbines Number of turbine links.
     * @param signalTypes Signals available for tapping and FDI.
     * @param clock Clock used for heartbeat tracking.
     * @param leaseTimeout Time before an inactive client expires.
     */

    StartAttackSessionResult startSession(std::string label);
    /**
     * Starts one attack client session.
     * @param label Human-readable attack label.
     * @return The new session ID or an error.
     */

    bool heartbeat();
    /**
     * Renews the active client lease.
     * @return True when a session is active.
     */

    std::optional<ClosedAttackSession> endSession(std::string reason);
    /**
     * Ends the active session and disables all attack controls.
     * @param reason Reason recorded for the closure.
     * @return Closed session details, or no value when inactive.
     */

    std::optional<ClosedAttackSession> expireSession();
    /**
     * Ends the active session when its lease has expired.
     * @return Closed session details when expiry occurred.
     */

    bool setTapEnabled(int turbineId, AttackInterface::SignalType signalType, bool enabled);
    /**
     * Enables or disables tapping on one turbine signal.
     * @param turbineId One-based turbine ID.
     * @param signalType Attack-interface signal type.
     * @param enabled New tap state.
     * @return True when the state was updated.
     */

    bool setFdiEnabled(int turbineId, AttackInterface::SignalType signalType, bool enabled);
    /**
     * Enables or disables FDI on one turbine signal.
     * @param turbineId One-based turbine ID.
     * @param signalType Attack-interface signal type.
     * @param enabled New FDI state.
     * @return True when the state was updated.
     */

    bool tapEnabled(int turbineId, AttackInterface::SignalType signalType) const;
    /**
     * Checks whether tapping is enabled.
     * @param turbineId One-based turbine ID.
     * @param signalType Attack-interface signal type.
     * @return True when tapping is active.
     */

    bool fdiEnabled(int turbineId, AttackInterface::SignalType signalType) const;
    /**
     * Checks whether FDI is enabled.
     * @param turbineId One-based turbine ID.
     * @param signalType Attack-interface signal type.
     * @return True when FDI is active.
     */

    bool setFdiValue(int turbineId, AttackInterface::SignalType signalType, float value);
    /**
     * Stores the latest FDI value for an enabled signal.
     * @param turbineId One-based turbine ID.
     * @param signalType Attack-interface signal type.
     * @param value Replacement value supplied by the client.
     * @return True when the value was stored.
     */

    std::optional<float> fdiValue(int turbineId, AttackInterface::SignalType signalType) const;
    /**
     * Gets the latest FDI value for an enabled signal.
     * @param turbineId One-based turbine ID.
     * @param signalType Attack-interface signal type.
     * @return Replacement value, or no value when unavailable.
     */

    std::optional<AttackSessionInfo> session() const;
    /**
     * Gets the active attack session.
     * @return Session information, or no value when inactive.
     */

private:
    struct LinkState {
        std::map<AttackInterface::SignalType, bool> tapEnabled;
        std::map<AttackInterface::SignalType, bool> fdiEnabled;
        std::map<AttackInterface::SignalType, std::optional<float>> fdiValues;
    };

    struct SessionState {
        AttackSessionInfo info;
        std::vector<LinkState> linkStates;
    };

    static bool validLabel(const std::string& label);
    /**
     * Checks whether an attack label is valid.
     * @param label Label to validate.
     * @return True when the label is valid.
     */

    bool validLinkLocked(int turbineId, AttackInterface::SignalType signalType) const;
    /**
     * Checks a turbine and signal while the state is locked.
     * @param turbineId One-based turbine ID.
     * @param signalType Attack-interface signal type.
     * @return True when the link and signal exist.
     */

    std::vector<LinkState> makeLinkStates() const;
    /**
     * Creates disabled tap and FDI state for every turbine link.
     * @return Initialized link states.
     */

    std::optional<ClosedAttackSession> endSessionLocked(std::string reason);
    /**
     * Ends the active session while the state is locked.
     * @param reason Reason recorded for the closure.
     * @return Closed session details, or no value when inactive.
     */

    int numTurbines_;
    std::vector<AttackInterface::SignalType> signalTypes_;
    sc::ports::Clock& clock_;
    std::chrono::milliseconds leaseTimeout_;
    mutable std::mutex mutex_;
    std::optional<SessionState> activeSession_;
    AttackSessionId nextSessionId_{1};
};

} // namespace sc::application
