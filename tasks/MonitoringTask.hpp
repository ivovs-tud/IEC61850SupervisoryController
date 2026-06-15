#pragma once

#include <cstdint>
#include <vector>

#include "common/PeriodicTask.hpp"

// ---------------------------------------------------------------------------
// MonitoringTask – system health monitoring + GOOSE subscriber.
// ---------------------------------------------------------------------------
class MonitoringTask : public PeriodicTask
{
public:
    explicit MonitoringTask(std::chrono::milliseconds period = std::chrono::milliseconds(50));

protected:

    // -----------------------------------------------------------
    // Variables used internally for monitoring logic and state
    // -----------------------------------------------------------
    // Orientation-Related.
    const float orientation_threshold = 8.0f; // Threshold for triggering the yaw misalignment alarm (in degrees)
    const float observer_gain = 0.5f; // Gain for a simple observer to estimate the true orientation based on measurements (placeholder value, needs tuning)
    const uint64_t yaw_measurement_timeout_ms = 1000; // Time after which we consider the yaw measurement to be outdated (in milliseconds)
    std::vector<uint64_t> last_yaw_measurement_time; // Timestamp of the last yaw measurement that was used for detection
    std::vector<uint64_t> last_orientation_prediction_time; // Timestamp of the last orientation prediction update
    std::vector<float> orientation_state;

    // Power tracking detector.
    const uint64_t power_tracking_grace_period_ms = 30000;
    const uint64_t power_measurement_timeout_ms = 2000;
    const double power_tracking_absolute_tolerance_w = 2.5e5;
    const double power_tracking_relative_tolerance = 0.05;
    std::vector<uint64_t> power_tracking_mismatch_start_time;
    std::vector<double> last_expected_power;

    uint64_t last_reset_ms = 0;


    void execute() override;

	// ---------------------------------------------------------------------------
    // Simple detection + consistency checks for monitored parameters
	// ---------------------------------------------------------------------------

    bool checkOutOfBoundsAll();
    /**
     * @brief Checks if any monitored parameters are out of their engineering bounds (static thresholds).
     * 
     * @return true if all parameters are within bounds, false otherwise.
     */

    bool checkConsistencyPowerGeneratedVsReceived();
    /**
        @brief Uses the moving average power generated as received from each turbine, 
            v.s. the 'locally measured total power' for consistency check.
    */

    bool checkConsistencyMeasuredPowerVsExpected();
    /**
        @brief Checks measured turbine power against min(power reference, available power).

        A mismatch is tolerated for a short transient window. The timer resets as
        soon as measured power returns close to the expected value.
    */

    bool checkConsistencyOrientationDynamics();
    /**
        @brief Checks for consistency between where the turbine is expected to point and what is received

        In particular, the prediction model advances the previous orientation toward the
        yaw setpoint by at most the turbine yawing rate times the elapsed monitor time.
    */

    bool checkConsistencyPowerTorqueRotorSpeed();
    /**
     * @brief Checks for consistency between power, torque, and rotor speed measurements using the relation 
     *  $$P = T \cdot \omega$$
     */

    bool checkConsistencyWindDirection();
    /**
     * @brief Checks for consistency between wind direction measurements and the global one. 
     *  This will in general be a very rough check of the order of 90 degrees.
     */

    bool checkConsistencyWindDirectionChange();
    /**
     * @brief Checks for consistency between change in wind direction over time. 
     * This will in general be a very rough check of the order of 5 degrees/s.
     */

    bool checkConsistencyWindSpeedChange();
    /**
     * @brief Checks for consistency between change in wind speed over time. 
     * This will in general be a very rough check of the order of 1 m/s^2.
     */

    /*bool checkConsistencyPowerGeneratedVsAvailable();
	bool checkConsistencyPowerReceivedVsAvailableAll();
    
    bool detectorWindSpeed();
    bool detectorWindDir();*/




private:
    // GOOSE message callback placeholder.
    // TODO: replace void* params with GooseSubscriber* / GooseMessage* from libiec61850
    //   once libiec_wrapper exposes the subscription API.
    static void onGooseMessage(void* subscriber, void* parameter);
};
