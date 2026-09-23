#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "communication/AttackInterface.hpp"
#include "communication/socket/AttackChannelTCP.hpp"

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: sc_raw_attack_loopback_server <port> [overflow]\n";
        return 2;
    }
    const bool overflowMode = argc == 3 && std::string(argv[2]) == "overflow";

    AttackChannelTCP::Config channelConfig;
    channelConfig.bindAddress = "127.0.0.1";
    channelConfig.port = std::atoi(argv[1]);
    channelConfig.pollPeriod = std::chrono::milliseconds(1);
    channelConfig.turbineCount = 2;
    if (overflowMode) channelConfig.transmitBufferBytes = 268;
    AttackChannelTCP channel(channelConfig);

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
        if (event.find("event=disconnected") != std::string::npos) {
            disconnectCount.fetch_add(1);
            running.store(false);
        }
    });
    channel.setFailureHandler([&](const std::string& message) {
        std::cerr << "raw attack server failure: " << message << '\n';
        running.store(false);
    });

    if (!channel.start()) {
        std::cerr << "failed to start raw attack server: " << channel.failureMessage() << '\n';
        return 3;
    }

    int overwriteSuccessCount = 0;
    int overwriteTimeoutCount = 0;
    bool overflowTriggered = false;
    while (running.load() && channel.isRunning()) {
        if (overflowMode) {
            if (configurationCount.load() > 0 && !overflowTriggered) {
                overflowTriggered = true;
                const std::vector<uint8_t> oversized(channelConfig.transmitBufferBytes + 1, 0);
                channel.send(oversized.data(), oversized.size());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        float yaw = 7.0F;
        attack.txData(1, AttackInterface::SignalType::YAW_ANGLE, &yaw);

        float yawSetpoint = -1.0F;
        const auto result = attack.overwrite(
            1, AttackInterface::SignalType::YAW_SETPOINT, yawSetpoint);
        if (result == AttackInterface::AI_OK) {
            ++overwriteSuccessCount;
            attack.txData(1, AttackInterface::SignalType::YAW_SETPOINT, &yawSetpoint);
        } else if (result == AttackInterface::AI_TIMEOUT) {
            ++overwriteTimeoutCount;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    channel.stop();
    std::cout << "SC_RAW_LOOPBACK_RESULT configurations=" << configurationCount.load()
              << " disconnects=" << disconnectCount.load()
              << " overwrite_successes=" << overwriteSuccessCount
              << " overwrite_timeouts=" << overwriteTimeoutCount << '\n';
    return channel.failure() ? 4 : 0;
}
