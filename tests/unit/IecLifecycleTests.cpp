#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "sc/communication/attack/AttackInterface.hpp"
#include "sc/communication/CommunicationConfig.hpp"
#include "sc/communication/iec61850/IEC61850Manager.hpp"
#include "sc/communication/iec61850/IECCommunicator.hpp"
#include "LibIecGooseReceiver.hpp"
#include "support/FakeAttackChannel.hpp"
#include "support/FakeClock.hpp"

using namespace std::chrono_literals;

struct IEC61850ManagerTestAccess {
    static std::shared_ptr<TurbineConnection> turbine(IEC61850Manager& manager, int turbineId) {
        return manager.findTurbine(turbineId);
    }

    static std::size_t reportCount(IEC61850Manager& manager) {
        std::lock_guard<std::mutex> lock(manager.reportMutex_);
        return manager.reportSubscriptions_.size();
    }

    static bool reportActive(IEC61850Manager& manager, int turbineId) {
        std::lock_guard<std::mutex> lock(manager.reportMutex_);
        for (const auto& [key, subscription] : manager.reportSubscriptions_) {
            (void)key;
            if (subscription->turbineId == turbineId) return subscription->active;
        }
        return false;
    }

    static std::size_t gooseSubscriptionCount(IEC61850Manager& manager) {
        std::lock_guard<std::mutex> lock(manager.gooseMutex_);
        return manager.gooseSubscriptions_.size();
    }
};

struct IECCommunicatorTestAccess {
    static void handleReportValues(IECCommunicator& communicator,
                                   int turbineId,
                                   const std::vector<IecReportValue>& values) {
        communicator.handleReportValues(turbineId, values);
    }

    static void startReporting(IECCommunicator& communicator) {
        communicator.startReporting();
    }

    static void stopReporting(IECCommunicator& communicator) {
        communicator.stopReporting();
    }

    static bool reportStarted(const IECCommunicator& communicator) {
        return communicator.reportStarted_.load();
    }

    static void executeRx(IECCommunicator& communicator) {
        communicator.executeRx();
    }

    static std::size_t scheduledRxCount(const IECCommunicator& communicator) {
        std::size_t count = 0;
        for (const auto executionTime : communicator.rxNextExecutionTimes_) {
            if (executionTime != 0) ++count;
        }
        return count;
    }

    static std::size_t bufferedReportCount(IECCommunicator& communicator) {
        std::lock_guard<std::mutex> lock(communicator.reportRxBufferMutex_);
        std::size_t count = 0;
        for (const auto& value : communicator.reportRxBuffer_) {
            if (value) ++count;
        }
        return count;
    }
};

namespace {

// TCP port zero cannot host a listening endpoint, so connection attempts fail
// locally without depending on test-machine port allocation.
constexpr int unavailableLocalPort = 0;

std::vector<IecReportValue> mappedReport(float value = 8.5F) {
    return {{"WMET1$MX$HorWdSpd", value, 1'234}};
}

} // namespace

TEST_CASE("IEC connection intent does not perform network I/O") {
    FakeClock clock;
    IEC61850Manager manager(clock, 100ms, 400ms);
    manager.addTurbine(1, "127.0.0.1", unavailableLocalPort);

    REQUIRE(manager.connectTurbine(1));

    const auto turbine = IEC61850ManagerTestAccess::turbine(manager, 1);
    REQUIRE(turbine);
    REQUIRE(turbine->connection == nullptr);
    REQUIRE(turbine->intentConnected);
    REQUIRE(turbine->status == IEC_LINK_RECONNECTING);
    REQUIRE(turbine->nextReconnectAttempt == sc::ports::Clock::SteadyTimePoint{});
    REQUIRE(turbine->reconnectDelay == 100ms);
}

TEST_CASE("IEC reconnect attempts are time-gated and capped") {
    FakeClock clock;
    IEC61850Manager manager(clock, 100ms, 400ms);
    manager.addTurbine(1, "127.0.0.1", unavailableLocalPort);
    REQUIRE(manager.connectTurbine(1));

    const auto turbine = IEC61850ManagerTestAccess::turbine(manager, 1);
    REQUIRE_FALSE(manager.readFloat(1, "LD0/MMXU1.TotW.mag.f", 1));
    REQUIRE(turbine->nextReconnectAttempt == clock.steadyNow() + 100ms);
    REQUIRE(turbine->reconnectDelay == 200ms);

    const auto firstDeadline = turbine->nextReconnectAttempt;
    REQUIRE_FALSE(manager.readFloat(1, "LD0/MMXU1.TotW.mag.f", 1));
    REQUIRE(turbine->nextReconnectAttempt == firstDeadline);
    REQUIRE(turbine->reconnectDelay == 200ms);

    clock.advance(100ms);
    REQUIRE_FALSE(manager.readFloat(1, "LD0/MMXU1.TotW.mag.f", 1));
    REQUIRE(turbine->nextReconnectAttempt == clock.steadyNow() + 200ms);
    REQUIRE(turbine->reconnectDelay == 400ms);

    clock.advance(200ms);
    REQUIRE_FALSE(manager.readFloat(1, "LD0/MMXU1.TotW.mag.f", 1));
    REQUIRE(turbine->nextReconnectAttempt == clock.steadyNow() + 400ms);
    REQUIRE(turbine->reconnectDelay == 400ms);
    REQUIRE(turbine->status == IEC_LINK_RECONNECTING);
}

TEST_CASE("IEC reconnect state is independent for each turbine") {
    FakeClock clock;
    IEC61850Manager manager(clock, 100ms, 400ms);
    manager.addTurbine(1, "127.0.0.1", unavailableLocalPort);
    manager.addTurbine(2, "127.0.0.1", unavailableLocalPort);
    manager.connectAll();

    const auto first = IEC61850ManagerTestAccess::turbine(manager, 1);
    const auto second = IEC61850ManagerTestAccess::turbine(manager, 2);
    REQUIRE_FALSE(manager.readFloat(1, "LD0/MMXU1.TotW.mag.f", 1));

    REQUIRE(first->nextReconnectAttempt == clock.steadyNow() + 100ms);
    REQUIRE(first->reconnectDelay == 200ms);
    REQUIRE(second->nextReconnectAttempt == sc::ports::Clock::SteadyTimePoint{});
    REQUIRE(second->reconnectDelay == 100ms);

    REQUIRE_FALSE(manager.readFloat(2, "LD0/MMXU1.TotW.mag.f", 1));
    REQUIRE(second->nextReconnectAttempt == clock.steadyNow() + 100ms);
    REQUIRE(second->reconnectDelay == 200ms);
}

TEST_CASE("IEC turbine operations use independent locks") {
    FakeClock clock;
    IEC61850Manager manager(clock);
    manager.addTurbine(1, "127.0.0.1", unavailableLocalPort);
    manager.addTurbine(2, "127.0.0.1", unavailableLocalPort);

    const auto first = IEC61850ManagerTestAccess::turbine(manager, 1);
    std::unique_lock<std::mutex> firstLock(first->mutex);
    auto firstStatus = std::async(std::launch::async, [&manager]() {
        return manager.status(1);
    });
    auto secondStatus = std::async(std::launch::async, [&manager]() {
        return manager.status(2);
    });

    const bool secondCompleted = secondStatus.wait_for(1s) == std::future_status::ready;
    const bool firstWaited = firstStatus.wait_for(20ms) == std::future_status::timeout;
    firstLock.unlock();

    REQUIRE(secondCompleted);
    REQUIRE(firstWaited);
    REQUIRE(secondStatus.get() == IEC_LINK_CLOSED);
    REQUIRE(firstStatus.get() == IEC_LINK_CLOSED);
}

TEST_CASE("IEC report configuration remains pending while disconnected") {
    FakeClock clock;
    IEC61850Manager manager(clock, 100ms, 400ms);
    manager.addTurbine(1, "127.0.0.1", unavailableLocalPort, "LD0", "IED1");
    REQUIRE(manager.connectTurbine(1));

    REQUIRE_FALSE(manager.startPeriodicReport(
        1,
        "LLN0$RP$Measurements",
        "LLN0$Measurements",
        500,
        {"MMXU1.TotW.mag.f"},
        [](int, const std::vector<IecReportValue>&) {}));
    REQUIRE(IEC61850ManagerTestAccess::reportCount(manager) == 1);
    REQUIRE_FALSE(IEC61850ManagerTestAccess::reportActive(manager, 1));

    REQUIRE_FALSE(manager.periodicReportActive(1, "LLN0$RP$Measurements"));
    REQUIRE(IEC61850ManagerTestAccess::reportCount(manager) == 1);
    REQUIRE_FALSE(IEC61850ManagerTestAccess::reportActive(manager, 1));

    manager.stopPeriodicReport(1, "LLN0$RP$Measurements");
    REQUIRE(IEC61850ManagerTestAccess::reportCount(manager) == 0);
}

TEST_CASE("IEC communicator falls back to polling while its report is pending") {
    SharedData::instance().configureTurbineCount(1);
    FakeClock clock;
    FakeAttackChannel channel;
    AttackInterface::AttackInterface attackInterface(1, channel, clock);
    std::mutex attackInterfaceMutex;
    IEC61850Manager manager(clock, 100ms, 400ms);
    manager.addTurbine(1, "127.0.0.1", unavailableLocalPort);
    REQUIRE(manager.connectTurbine(1));

    CommunicationConfig config;
    config.mms.reportingEnabled = true;
    config.mms.reportControlBlockReference = "LLN0$RP$Measurements";
    config.mms.reportDataSetReference = "LLN0$Measurements";
    IECCommunicator communicator(config, 1, manager, attackInterface, attackInterfaceMutex);

    IECCommunicatorTestAccess::startReporting(communicator);
    REQUIRE_FALSE(IECCommunicatorTestAccess::reportStarted(communicator));
    REQUIRE(IEC61850ManagerTestAccess::reportCount(manager) == 1);

    IECCommunicatorTestAccess::executeRx(communicator);
    REQUIRE(IECCommunicatorTestAccess::scheduledRxCount(communicator) == 6);
    REQUIRE(communicator.lastActivityTime() == std::chrono::system_clock::time_point{});

    IECCommunicatorTestAccess::stopReporting(communicator);
    REQUIRE(IEC61850ManagerTestAccess::reportCount(manager) == 0);
}

TEST_CASE("IEC communicator records activity only for accepted report values") {
    SharedData::instance().configureTurbineCount(1);
    FakeClock clock;
    FakeAttackChannel channel;
    AttackInterface::AttackInterface attackInterface(1, channel, clock);
    std::mutex attackInterfaceMutex;
    IEC61850Manager manager(clock);
    manager.addTurbine(1, "127.0.0.1", unavailableLocalPort);

    CommunicationConfig config;
    config.mms.reportingEnabled = false;
    IECCommunicator communicator(config, 1, manager, attackInterface, attackInterfaceMutex);
    const auto noActivity = std::chrono::system_clock::time_point{};

    REQUIRE(communicator.lastActivityTime() == noActivity);
    IECCommunicatorTestAccess::handleReportValues(communicator, 2, mappedReport());
    REQUIRE(communicator.lastActivityTime() == noActivity);
    IECCommunicatorTestAccess::handleReportValues(
        communicator, 1, {{"unknown", 1.0F, 1'234}});
    REQUIRE(communicator.lastActivityTime() == noActivity);
    REQUIRE(IECCommunicatorTestAccess::bufferedReportCount(communicator) == 0);

    IECCommunicatorTestAccess::handleReportValues(communicator, 1, mappedReport());
    REQUIRE(communicator.lastActivityTime() != noActivity);
    REQUIRE(communicator.status() == COMM_CONNECTED);
    REQUIRE(IECCommunicatorTestAccess::bufferedReportCount(communicator) == 1);
}

TEST_CASE("GOOSE resources can be configured and stopped repeatedly without starting reception") {
    LibIecGooseReceiver wrapper;
    REQUIRE(wrapper.configureGooseReceiver("test-interface"));
    REQUIRE(wrapper.addGooseSubscriber(
        "IED1LD0/LLN0$GO$TurbineState",
        1000,
        [](const std::string&, int32_t) {}));
    REQUIRE_FALSE(wrapper.gooseReceiverRunning());

    wrapper.stopGooseReceiver();
    wrapper.stopGooseReceiver();
    REQUIRE_FALSE(wrapper.gooseReceiverRunning());

    REQUIRE(wrapper.configureGooseReceiver("test-interface"));
    wrapper.stopGooseReceiver();
    REQUIRE_FALSE(wrapper.gooseReceiverRunning());
}

TEST_CASE("GOOSE configuration retains desired turbine subscriptions") {
    FakeClock clock;
    IEC61850Manager manager(clock);
    manager.addTurbine(1, "127.0.0.1", unavailableLocalPort, "LD0", "IED1");

    REQUIRE_FALSE(manager.configureGoose(""));
    REQUIRE_FALSE(manager.addGooseSubscription(
        2, "LLN0$GO$TurbineState", [](const std::string&, int32_t) {}));
    REQUIRE(manager.addGooseSubscription(
        1, "LLN0$GO$TurbineState", [](const std::string&, int32_t) {}));
    REQUIRE(IEC61850ManagerTestAccess::gooseSubscriptionCount(manager) == 1);

    REQUIRE(manager.configureGoose("first-interface"));
    REQUIRE(manager.configureGoose("second-interface"));
    REQUIRE(IEC61850ManagerTestAccess::gooseSubscriptionCount(manager) == 1);
    REQUIRE_FALSE(manager.gooseRunning());

    manager.stopGoose();
    manager.stopGoose();
    REQUIRE(IEC61850ManagerTestAccess::gooseSubscriptionCount(manager) == 1);
    REQUIRE_FALSE(manager.gooseRunning());
}
