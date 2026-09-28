#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "sc/communication/attack/AttackInterface.hpp"
#include "sc/communication/attack/AttackChannelTCP.hpp"

namespace {

enum class Mode {
    Normal,
    Overflow,
    LeaseTimeout,
    Shutdown,
    SlowReader,
};

Mode parseMode(int argc, char* argv[]) {
    if (argc == 2) return Mode::Normal;
    const std::string value = argv[2];
    if (value == "overflow") return Mode::Overflow;
    if (value == "lease-timeout") return Mode::LeaseTimeout;
    if (value == "shutdown") return Mode::Shutdown;
    if (value == "slow-reader") return Mode::SlowReader;
    throw std::invalid_argument("unknown raw attack loopback mode: " + value);
}

int disconnectReasonCode(const std::string& reason) {
    if (reason == "client release") return 1;
    if (reason == "peer closed connection") return 2;
    if (reason.find("receive error") != std::string::npos || reason == "connection closed") return 3;
    if (reason.find("protocol error") != std::string::npos) return 4;
    if (reason == "transmit buffer overflow") return 5;
    if (reason == "lease timeout") return 6;
    if (reason == "server shutdown") return 7;
    return 99;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: sc_raw_attack_loopback_server <port> "
                     "[overflow|lease-timeout|shutdown|slow-reader]\n";
        return 2;
    }
    Mode mode;
    try {
        mode = parseMode(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }

    AttackChannelTCP::Config channelConfig;
    channelConfig.bindAddress = "127.0.0.1";
    channelConfig.port = std::atoi(argv[1]);
    channelConfig.pollPeriod = std::chrono::milliseconds(1);
    channelConfig.turbineCount = 2;
    if (mode == Mode::Overflow) channelConfig.transmitBufferBytes = 268;
    if (mode == Mode::SlowReader) channelConfig.transmitBufferBytes = 1024;
    AttackChannelTCP channel(channelConfig);

    AttackInterface::AttackTiming timing;
    timing.requestLifetime = std::chrono::milliseconds(200);
    timing.requestRetryPeriod = std::chrono::milliseconds(100);
    if (mode == Mode::LeaseTimeout) {
        timing.sessionLeaseTimeout = std::chrono::milliseconds(250);
    }
    AttackInterface::AttackInterface attack(2, channel, sc::ports::systemClock(), timing);

    std::atomic<bool> running{true};
    std::atomic<int> configurationCount{0};
    std::atomic<int> disconnectCount{0};
    std::atomic<bool> attackStarted{false};
    std::chrono::steady_clock::time_point configuredAt{};
    std::chrono::steady_clock::time_point disconnectedAt{};
    std::mutex disconnectMutex;
    std::string disconnectReason;
    attack.setAuditCallback([&](const std::string& event) {
        if (event.find("event=connected") != std::string::npos) {
            configurationCount.fetch_add(1);
            configuredAt = std::chrono::steady_clock::now();
        }
        if (event.find("event=attack_started") != std::string::npos) {
            attackStarted.store(true);
            if (mode == Mode::SlowReader) {
                for (int index = 0; index < 256; ++index) {
                    float yaw = static_cast<float>(index);
                    attack.txData(1, AttackInterface::SignalType::YAW_ANGLE, &yaw);
                }
            }
        }
        if (event.find("event=disconnected") != std::string::npos) {
            disconnectCount.fetch_add(1);
            disconnectedAt = std::chrono::steady_clock::now();
            const auto reason = event.find("reason=");
            if (reason != std::string::npos) {
                std::lock_guard<std::mutex> lock(disconnectMutex);
                disconnectReason = event.substr(reason + 7);
            }
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
    bool modeTriggered = false;
    while (running.load() && channel.isRunning()) {
        if (mode == Mode::Overflow) {
            if (attackStarted.load() && !modeTriggered) {
                modeTriggered = true;
                const std::vector<uint8_t> oversized(channelConfig.transmitBufferBytes + 1, 0);
                channel.send(oversized.data(), oversized.size());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (mode == Mode::Shutdown && attackStarted.load() && !modeTriggered) {
            modeTriggered = true;
            channel.stop();
            continue;
        }
        if (mode == Mode::SlowReader) {
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
    float authoritativeValue = 321.0F;
    const bool restored =
        attack.overwrite(1, AttackInterface::SignalType::YAW_SETPOINT, authoritativeValue) ==
            AttackInterface::AI_DISABLED &&
        authoritativeValue == 321.0F;
    std::string finalReason;
    {
        std::lock_guard<std::mutex> lock(disconnectMutex);
        finalReason = disconnectReason;
    }
    const auto leaseElapsed = configuredAt == std::chrono::steady_clock::time_point{} ||
                                      disconnectedAt == std::chrono::steady_clock::time_point{}
        ? -1
        : std::chrono::duration_cast<std::chrono::milliseconds>(
              disconnectedAt - configuredAt).count();
    std::cout << "SC_RAW_LOOPBACK_RESULT configurations=" << configurationCount.load()
              << " disconnects=" << disconnectCount.load()
              << " overwrite_successes=" << overwriteSuccessCount
              << " overwrite_timeouts=" << overwriteTimeoutCount
              << " restored=" << static_cast<int>(restored)
              << " reason=" << disconnectReasonCode(finalReason)
              << " lease_elapsed_ms=" << leaseElapsed << '\n';
    return channel.failure() ? 4 : 0;
}
