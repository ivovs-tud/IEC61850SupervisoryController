#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "communication/AttackInterface.hpp"
#include "communication/socket/SocketWrapper.hpp"

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "usage: sc_legacy_attack_loopback_server <port>\n";
        return 2;
    }

    const int port = std::atoi(argv[1]);
    SocketWrapper channel;
    AttackInterface::AttackTiming timing;
    timing.requestLifetime = std::chrono::milliseconds(200);
    timing.responseTimeout = std::chrono::milliseconds(100);
    timing.responsePollPeriod = std::chrono::milliseconds(5);
    AttackInterface::AttackInterface attack(2, channel, sc::ports::systemClock(), timing);

    std::atomic<bool> running{true};
    std::atomic<int> scenarioId{0};
    std::atomic<int> configurationCount{0};
    attack.setCfgCommandCallback([&](const AttackInterface::CfgDataMessage& message) {
        scenarioId.store(message.scenarioId);
        configurationCount.fetch_add(1);
    });
    attack.setSimCtrlCommandCallback([&](const AttackInterface::SimCtrlMessage& message) {
        if (!message.simStart) running.store(false);
    });

    if (channel.StartAttackInterfaceServer(port) < tcpSOCKET_CONNECTED) {
        std::cerr << "failed to start legacy attack server\n";
        return 3;
    }

    int overwriteSuccessCount = 0;
    int overwriteTimeoutCount = 0;
    while (running.load()) {
        float yaw = static_cast<float>(scenarioId.load());
        attack.txData(1, AttackInterface::TX_YAW, &yaw);

        float yawSetpoint = -1.0F;
        const auto result = attack.overwrite(1, AttackInterface::TX_SPT_YAW, yawSetpoint);
        if (result == AttackInterface::AI_OK) {
            ++overwriteSuccessCount;
            attack.txData(1, AttackInterface::TX_SPT_YAW, &yawSetpoint);
        } else if (result == AttackInterface::AI_TIMEOUT) {
            ++overwriteTimeoutCount;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    channel.StopAttackInterfaceServer();
    std::cout << "SC_LOOPBACK_RESULT configurations=" << configurationCount.load()
              << " overwrite_successes=" << overwriteSuccessCount
              << " overwrite_timeouts=" << overwriteTimeoutCount << '\n';
    return 0;
}
