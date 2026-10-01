#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sc::application {

struct TurbineMonitoringInput {
    double windSpeed{0.0};
    uint64_t windSpeedTimeMs{0};
    double filteredWindSpeed{0.0};
    uint64_t filteredWindSpeedTimeMs{0};
    double windDirection{0.0};
    uint64_t windDirectionTimeMs{0};
    double yaw{0.0};
    uint64_t yawTimeMs{0};
    double rotorSpeed{0.0};
    uint64_t rotorSpeedTimeMs{0};
    double power{0.0};
    uint64_t powerTimeMs{0};
    double generatorTorque{0.0};
    uint64_t generatorTorqueTimeMs{0};
    float powerSetpoint{-1.0F};
    float yawSetpoint{0.0F};
    bool maximumPowerMode{true};
    bool commandedOff{false};
    std::vector<double> windSpeedHistory;
    std::vector<double> windDirectionHistory;
    std::vector<double> rotorSpeedHistory;
    std::vector<double> powerHistory;
    std::vector<double> generatorTorqueHistory;
};

struct MonitoringInput {
    uint64_t currentTimeMs{0};
    int connectedTurbines{0};
    float farmWindSpeed{0.0F};
    float farmWindDirection{0.0F};
    std::vector<double> measuredTotalPowerHistory;
    std::vector<TurbineMonitoringInput> turbines;
};

enum class AlarmType : std::size_t {
    PowerGeneratedVsReceived,
    MeasuredPowerVsExpected,
    OrientationMisalignment,
    PowerTorqueRotorSpeed,
    WindDirection,
    WindDirectionChange,
    WindSpeedChange,
    TelemetryFreeze,
    DrivetrainUnderResponse,
    StaticTelemetryBounds,
    FleetPeerOutlier,
    Count
};

struct AlarmEvidence {
    AlarmType alarm;
    int turbineId{0}; // Zero denotes a farm-level alarm.
    double measured{0.0};
    double expected{0.0};
    double threshold{0.0};
    uint64_t timestampMs{0};
};

struct MonitoringResult {
    std::array<bool, static_cast<std::size_t>(AlarmType::Count)> active{};
    std::vector<AlarmEvidence> evidence;

    bool isActive(AlarmType alarm) const;
};

struct AlarmState {
    bool active{false};
    bool latched{false};
    bool acknowledged{false};
};

class AlarmStateTracker {
public:
    void update(const MonitoringResult& result);
    void acknowledge();
    void reset();

    const AlarmState& state(AlarmType alarm) const;
    bool visible(AlarmType alarm) const;

private:
    std::array<AlarmState, static_cast<std::size_t>(AlarmType::Count)> states_{};
};

// Owns detector history; input and output contain no shared-state references.
class MonitoringDetectors {
public:
    explicit MonitoringDetectors(std::size_t turbineCount);

    MonitoringResult evaluate(const MonitoringInput& input);
    MonitoringResult evaluateDetector(AlarmType alarm, const MonitoringInput& input);
    void reset();

private:
    void checkPowerBalance(const MonitoringInput& input, MonitoringResult& result) const;
    void checkPowerTracking(const MonitoringInput& input, MonitoringResult& result);
    void checkOrientation(const MonitoringInput& input, MonitoringResult& result);
    void checkPowerTorqueRotorSpeed(const MonitoringInput& input, MonitoringResult& result) const;
    void checkWindDirection(const MonitoringInput& input, MonitoringResult& result) const;
    void checkWindDirectionChange(const MonitoringInput& input, MonitoringResult& result);
    void checkWindSpeedChange(const MonitoringInput& input, MonitoringResult& result);
    void checkTelemetryFreeze(const MonitoringInput& input, MonitoringResult& result);
    void checkDrivetrainResponse(const MonitoringInput& input, MonitoringResult& result);
    void checkStaticBounds(const MonitoringInput& input, MonitoringResult& result) const;
    void checkFleetPeerOutliers(const MonitoringInput& input, MonitoringResult& result);

    std::size_t turbineCount_;
    std::vector<uint64_t> lastYawMeasurementTime_;
    std::vector<uint64_t> lastOrientationPredictionTime_;
    std::vector<float> orientation_;
    std::vector<uint64_t> powerMismatchStartTime_;
    std::vector<double> lastExpectedPower_;
    std::vector<std::vector<double>> expectedPowerHistory_;
    std::vector<int> windSpeedChangeStrikes_;
    std::vector<int> windDirectionChangeStrikes_;
    std::vector<std::array<uint64_t, 5>> freezeStartTime_;
    std::vector<uint64_t> drivetrainUnderResponseStartTime_;
    std::vector<uint64_t> fleetPeerOutlierStartTime_;
};

} // namespace sc::application
