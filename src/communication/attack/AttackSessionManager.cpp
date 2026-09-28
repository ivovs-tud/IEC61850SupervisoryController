#include "sc/communication/attack/AttackSessionManager.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace sc::application {

AttackSessionManager::AttackSessionManager(int numTurbines,
                                           std::vector<AttackInterface::SignalType> signalTypes,
                                           sc::ports::Clock& clock,
                                           std::chrono::milliseconds leaseTimeout)
    : numTurbines_(numTurbines),
      signalTypes_(std::move(signalTypes)),
      clock_(clock),
      leaseTimeout_(leaseTimeout) {
    if (numTurbines_ <= 0) throw std::invalid_argument("number of turbines must be positive");
    if (signalTypes_.empty()) throw std::invalid_argument("attack signal types must not be empty");
    if (leaseTimeout_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("attack session lease timeout must be positive");
    }

    std::sort(signalTypes_.begin(), signalTypes_.end());
    signalTypes_.erase(std::unique(signalTypes_.begin(), signalTypes_.end()), signalTypes_.end());
}

StartAttackSessionResult AttackSessionManager::startSession(std::string label) {
    if (!validLabel(label)) return {0, "attack label must contain printable non-whitespace characters"};

    std::lock_guard<std::mutex> lock(mutex_);
    if (activeSession_) return {0, "an attack client session is already active"};
    const AttackSessionId id = nextSessionId_++;
    activeSession_ = SessionState{
        AttackSessionInfo{id, std::move(label), clock_.steadyNow()},
        makeLinkStates()};
    return {id, {}};
}

bool AttackSessionManager::heartbeat() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!activeSession_) return false;
    activeSession_->info.lastHeartbeat = clock_.steadyNow();
    return true;
}

std::optional<ClosedAttackSession> AttackSessionManager::endSession(std::string reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    return endSessionLocked(std::move(reason));
}

std::optional<ClosedAttackSession> AttackSessionManager::expireSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!activeSession_ ||
        clock_.steadyNow() - activeSession_->info.lastHeartbeat < leaseTimeout_) {
        return std::nullopt;
    }
    return endSessionLocked("lease timeout");
}

bool AttackSessionManager::setTapEnabled(int turbineId, AttackInterface::SignalType signalType, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!activeSession_ || !validLinkLocked(turbineId, signalType)) return false;
    activeSession_->linkStates[static_cast<size_t>(turbineId - 1)].tapEnabled[signalType] = enabled;
    return true;
}

bool AttackSessionManager::setFdiEnabled(int turbineId, AttackInterface::SignalType signalType, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!activeSession_ || !validLinkLocked(turbineId, signalType)) return false;
    activeSession_->linkStates[static_cast<size_t>(turbineId - 1)].fdiEnabled[signalType] = enabled;
    if (!enabled) {
        activeSession_->linkStates[static_cast<size_t>(turbineId - 1)].fdiValues[signalType].reset();
    }
    return true;
}

bool AttackSessionManager::tapEnabled(int turbineId, AttackInterface::SignalType signalType) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!activeSession_ || !validLinkLocked(turbineId, signalType)) return false;
    return activeSession_->linkStates[static_cast<size_t>(turbineId - 1)].tapEnabled.at(signalType);
}

bool AttackSessionManager::fdiEnabled(int turbineId, AttackInterface::SignalType signalType) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!activeSession_ || !validLinkLocked(turbineId, signalType)) return false;
    return activeSession_->linkStates[static_cast<size_t>(turbineId - 1)].fdiEnabled.at(signalType);
}

bool AttackSessionManager::setFdiValue(int turbineId,
                                       AttackInterface::SignalType signalType,
                                       float value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!activeSession_ || !validLinkLocked(turbineId, signalType)) return false;
    auto& link = activeSession_->linkStates[static_cast<size_t>(turbineId - 1)];
    if (!link.fdiEnabled.at(signalType)) return false;
    link.fdiValues[signalType] = value;
    return true;
}

std::optional<float> AttackSessionManager::fdiValue(
    int turbineId,
    AttackInterface::SignalType signalType) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!activeSession_ || !validLinkLocked(turbineId, signalType)) return std::nullopt;
    const auto& link = activeSession_->linkStates[static_cast<size_t>(turbineId - 1)];
    if (!link.fdiEnabled.at(signalType)) return std::nullopt;
    return link.fdiValues.at(signalType);
}

std::optional<AttackSessionInfo> AttackSessionManager::session() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!activeSession_) return std::nullopt;
    return activeSession_->info;
}

bool AttackSessionManager::validLabel(const std::string& label) {
    if (label.empty() || label.size() > 255) return false;
    bool hasVisibleCharacter = false;
    for (unsigned char character : label) {
        if (character < 0x20 || character == 0x7f) return false;
        if (!std::isspace(character)) hasVisibleCharacter = true;
    }
    return hasVisibleCharacter;
}

bool AttackSessionManager::validLinkLocked(int turbineId, AttackInterface::SignalType signalType) const {
    if (!activeSession_ || turbineId < 1 || turbineId > numTurbines_) return false;
    const auto& tapEnabled =
        activeSession_->linkStates[static_cast<size_t>(turbineId - 1)].tapEnabled;
    return tapEnabled.find(signalType) != tapEnabled.end();
}

std::vector<AttackSessionManager::LinkState> AttackSessionManager::makeLinkStates() const {
    std::vector<LinkState> linkStates(static_cast<size_t>(numTurbines_));
    for (auto& linkState : linkStates) {
        for (AttackInterface::SignalType signalType : signalTypes_) {
            linkState.tapEnabled.emplace(signalType, false);
            linkState.fdiEnabled.emplace(signalType, false);
            linkState.fdiValues.emplace(signalType, std::nullopt);
        }
    }
    return linkStates;
}

std::optional<ClosedAttackSession> AttackSessionManager::endSessionLocked(std::string reason) {
    if (!activeSession_) return std::nullopt;
    ClosedAttackSession closed{
        activeSession_->info.id,
        activeSession_->info.label,
        std::move(reason)};
    activeSession_.reset();
    return closed;
}

} // namespace sc::application
