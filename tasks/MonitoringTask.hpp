#pragma once

#include <cmath>

#include "common/PeriodicTask.hpp"

/**
 * Periodic consistency checks over shared farm measurements.
 *
 * The current checks are intentionally simple threshold detectors. They are
 * useful as lightweight sanity alarms and as scaffolding for model-based
 * monitoring, but they are not a substitute for certified protection logic.
 */
class MonitoringTask : public PeriodicTask {
public:
    explicit MonitoringTask(std::chrono::milliseconds period = std::chrono::milliseconds(50));

protected:
    // First-order yaw observer parameters. The model assumes setpoint changes
    // are smooth enough that a 10 s yaw time constant is a useful sanity check.
    const float orientationTimeConstant_ = 10.0f;
    const float orientationAlpha_ = exp(-period_.count() / 1000.0f / orientationTimeConstant_);
    const float orientationThresholdDeg_ = 5.0f;
    const float orientationObserverGain_ = 0.5f;
    static constexpr uint64_t kYawMeasurementFreshnessMs = 1000;
    std::vector<uint64_t> lastYawMeasurementTimeMs_;
    std::vector<float> orientationState_;

    uint64_t lastResetMs_ = 0;

    void execute() override;

    /** Detects measurements outside static engineering limits. */
    bool hasOutOfBoundsMeasurements();

    /** Compares turbine-reported total power with locally accumulated power. */
    bool hasPowerReceivedMismatch();

    /**
     * Checks whether measured yaw follows a first-order response to setpoints.
     *
     * The observer update is:
     *   psi_hat[k+1] = (1 - alpha) psi_hat[k] + alpha psi_setpoint[k]
     */
    bool hasOrientationDynamicsMismatch();

    /** Uses the mechanical relation P ~= torque * rotor speed as a plausibility check. */
    bool hasPowerTorqueRotorSpeedMismatch();

    /** Flags turbines whose local wind direction is far from the farm estimate. */
    bool hasWindDirectionMismatch();

    /** Flags unrealistically abrupt wind-direction changes in per-turbine history. */
    bool hasWindDirectionStepChange();

    /** Flags unrealistically abrupt wind-speed changes in per-turbine history. */
    bool hasWindSpeedStepChange();

private:
    // Placeholder until LibIecWrapper exposes typed GOOSE subscription values.
    static void onGooseMessage(void* subscriber, void* parameter);
};
