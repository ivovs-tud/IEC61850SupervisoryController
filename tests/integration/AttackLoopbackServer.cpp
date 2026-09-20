#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "communication/AttackInterface.hpp"
#include "communication/socket/SocketWrapper.hpp"

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "usage: sc_attack_loopback_server <port>\n";
        return 2;
    }

    const int port = std::atoi(argv[1]);
    SocketWrapper channel;
    AttackInterface::AttackTiming timing;
    timing.requestLifetime = std::chrono::milliseconds(200);
    timing.requestRetryPeriod = std::chrono::milliseconds(100);
    AttackInterface::AttackInterface attack(2, channel, sc::ports::systemClock(), timing);

    std::atomic<bool> running{true};
    std::atomic<int> configurationCount{0};
    attack.setAuditCallback([&](const std::string& event) {
        if (event.find("event=connected") != std::string::npos) {
            configurationCount.fetch_add(1);
        }
        if (event.find("event=disconnected") != std::string::npos) {
            running.store(false);
        }
    });

    if (channel.StartAttackInterfaceServer(port) < tcpSOCKET_CONNECTED) {
        std::cerr << "failed to start attack server\n";
        return 3;
    }

    int overwriteSuccessCount = 0;
    int overwriteTimeoutCount = 0;
    while (running.load()) {
        float yaw = 7.0F;
        attack.txData(1, AttackInterface::SignalType::YAW_ANGLE, &yaw);

        float yawSetpoint = -1.0F;
        const auto result = attack.overwrite(1, AttackInterface::SignalType::YAW_SETPOINT, yawSetpoint);
        if (result == AttackInterface::AI_OK) {
            ++overwriteSuccessCount;
            attack.txData(1, AttackInterface::SignalType::YAW_SETPOINT, &yawSetpoint);
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
