#pragma once

#include <cstdint>
#include <vector>

namespace sc::application {

struct TurbineSignalInput {
    double windSpeed{0.0};
    uint64_t windSpeedTimeMs{0};
    double windDirection{0.0};
    uint64_t windDirectionTimeMs{0};
    double receivedPower{0.0};
    double measuredPower{0.0};
    uint64_t newestMeasurementTimeMs{0};
};

struct SignalProcessingInput {
    uint64_t currentTimeMs{0};
    float previousWindSpeed{0.0F};
    float previousWindDirection{0.0F};
    std::vector<TurbineSignalInput> turbines;
};

struct SignalProcessingConfig {
    uint64_t measurementTimeoutMs{2000};
    float windSpeedUpdateWeight{0.1F};
    float windDirectionUpdateWeight{0.05F};
};

struct SignalProcessingResult {
    int connectedTurbines{0};
    std::vector<double> availablePower;
    double totalReceivedPower{0.0};
    double totalMeasuredPower{0.0};
    float windSpeed{0.0F};
    float windDirection{0.0F};
};

double calculateAvailablePower(double windSpeed);
SignalProcessingResult processSignals(const SignalProcessingInput& input, const SignalProcessingConfig& config = {});
} // namespace sc::application
