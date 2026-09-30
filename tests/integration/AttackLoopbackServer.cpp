#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "sc/communication/attack/AttackInterface.hpp"
#include "sc/communication/attack/AttackChannelZMQ.hpp"

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: sc_attack_loopback_server <port> [slow-reader|burst]\n";
        return 2;
    }
    const bool slowReaderMode = argc == 3 && std::string(argv[2]) == "slow-reader";
    const bool burstMode = argc == 3 && std::string(argv[2]) == "burst";
    if (argc == 3 && !slowReaderMode && !burstMode) {
        std::cerr << "unknown attack loopback mode: " << argv[2] << '\n';
        return 2;
    }

    const int port = std::atoi(argv[1]);
    AttackChannelZMQ::Config channelConfig;
    channelConfig.port = port;
    channelConfig.pollPeriod = std::chrono::milliseconds(burstMode ? 100 : 1);
    channelConfig.turbineCount = 2;
    channelConfig.transmitBufferBytes = slowReaderMode ? 1024 : 64 * 1024;
    AttackChannelZMQ channel(channelConfig);
    AttackInterface::AttackTiming timing;
    timing.requestLifetime = std::chrono::milliseconds(200);
    AttackInterface::AttackInterface attack(2, channel, sc::ports::systemClock(), timing);

    std::atomic<bool> running{true};
    std::atomic<int> configurationCount{0};
    std::atomic<int> disconnectCount{0};
    std::atomic<int> controlChangeCount{0};
    std::atomic<bool> releasedByClient{false};
    attack.setAuditCallback([&](const std::string& event) {
        if (event.find("event=connected") != std::string::npos) {
            configurationCount.fetch_add(1);
        }
        if (slowReaderMode && event.find("event=attack_started") != std::string::npos) {
            for (int index = 0; index < 256; ++index) {
                float yaw = static_cast<float>(index);
                attack.txData(1, AttackInterface::SignalType::YAW_ANGLE, yaw);
            }
        }
        if (event.find("event=tap;") != std::string::npos ||
            event.find("event=fdi;") != std::string::npos) {
            controlChangeCount.fetch_add(1);
        }
        if (event.find("event=disconnected") != std::string::npos) {
            releasedByClient.store(event.find("reason=client release") != std::string::npos);
            disconnectCount.fetch_add(1);
            running.store(false);
        }
    });

    if (!channel.start()) {
        std::cerr << "failed to start attack server\n";
        return 3;
    }

    int overwriteSuccessCount = 0;
    int overwriteTimeoutCount = 0;
    while (running.load()) {
        if (burstMode) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        float yaw = 7.0F;
        attack.txData(1, AttackInterface::SignalType::YAW_ANGLE, yaw);

        float yawSetpoint = -1.0F;
        const auto result = attack.processValue(
            1, AttackInterface::SignalType::YAW_SETPOINT, yawSetpoint);
        if (result == AttackInterface::AI_OK) {
            ++overwriteSuccessCount;
        } else if (result == AttackInterface::AI_TIMEOUT) {
            ++overwriteTimeoutCount;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    channel.stop();
    float authoritativeValue = 321.0F;
    const bool restored =
        attack.overwrite(1, AttackInterface::SignalType::YAW_SETPOINT, authoritativeValue) ==
            AttackInterface::AI_DISABLED &&
        authoritativeValue == 321.0F;
    std::cout << "SC_LOOPBACK_RESULT configurations=" << configurationCount.load()
              << " disconnects=" << disconnectCount.load()
              << " overwrite_successes=" << overwriteSuccessCount
              << " overwrite_timeouts=" << overwriteTimeoutCount
              << " control_changes=" << controlChangeCount.load()
              << " client_release=" << static_cast<int>(releasedByClient.load())
              << " restored=" << static_cast<int>(restored) << '\n';
    return 0;
}
