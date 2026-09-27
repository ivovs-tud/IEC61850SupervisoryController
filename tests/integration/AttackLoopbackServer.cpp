#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "AttackInterface.hpp"
#include "SocketWrapper.hpp"

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: sc_attack_loopback_server <port> [slow-reader]\n";
        return 2;
    }
    const bool slowReaderMode = argc == 3 && std::string(argv[2]) == "slow-reader";
    if (argc == 3 && !slowReaderMode) {
        std::cerr << "unknown attack loopback mode: " << argv[2] << '\n';
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
    std::atomic<int> disconnectCount{0};
    attack.setAuditCallback([&](const std::string& event) {
        if (event.find("event=connected") != std::string::npos) {
            configurationCount.fetch_add(1);
        }
        if (slowReaderMode && event.find("event=attack_started") != std::string::npos) {
            for (int index = 0; index < 256; ++index) {
                float yaw = static_cast<float>(index);
                attack.txData(1, AttackInterface::SignalType::YAW_ANGLE, &yaw);
            }
        }
        if (event.find("event=disconnected") != std::string::npos) {
            disconnectCount.fetch_add(1);
            running.store(false);
        }
    });

    channel.ConfigureAttackInterface(
        64 * 1024,
        slowReaderMode ? 1024 : 64 * 1024,
        std::chrono::milliseconds(200),
        std::chrono::milliseconds(750));

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
    float authoritativeValue = 321.0F;
    const bool restored =
        attack.overwrite(1, AttackInterface::SignalType::YAW_SETPOINT, authoritativeValue) ==
            AttackInterface::AI_DISABLED &&
        authoritativeValue == 321.0F;
    std::cout << "SC_LOOPBACK_RESULT configurations=" << configurationCount.load()
              << " disconnects=" << disconnectCount.load()
              << " overwrite_successes=" << overwriteSuccessCount
              << " overwrite_timeouts=" << overwriteTimeoutCount
              << " restored=" << static_cast<int>(restored) << '\n';
    return 0;
}
