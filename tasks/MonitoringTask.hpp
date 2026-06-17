#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <vector>

#include "common/GlobalDataStructure.hpp"
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
    const uint64_t power_tracking_grace_period_ms = 5000;
    const uint64_t power_measurement_timeout_ms = 2000;
    const double power_tracking_absolute_tolerance_w = 5e5;
    const double power_tracking_relative_tolerance = 0.05;
    std::vector<uint64_t> power_tracking_mismatch_start_time;
    std::vector<double> last_expected_power;
    TurbineHistory<double> expected_power_history;

    // Wind-change detectors use short-window evidence and persistence to avoid
    // alarming on normal turbulence or one noisy sample.
    const double wind_speed_step_threshold_ms = 3.0;
    const double wind_speed_range_threshold_ms = 4.0;
    const double wind_direction_step_threshold_deg = 25.0;
    const double wind_direction_range_threshold_deg = 40.0;
    const int wind_change_required_strikes = 3;
    std::vector<int> wind_speed_change_strike_count;
    std::vector<int> wind_direction_change_strike_count;

    // Telemetry freeze detector for plausible-looking false data.
    const std::size_t telemetry_freeze_window_samples = 8;
    const uint64_t telemetry_freeze_persistence_ms = 1500;
    const uint64_t telemetry_freeze_measurement_timeout_ms = 3000;
    const double telemetry_freeze_ws_range_ms = 0;
    const double telemetry_freeze_wd_range_deg = 0;
    const double telemetry_freeze_yaw_range_deg = 0;
    const double telemetry_freeze_rpm_range = 0;
    const double telemetry_freeze_power_range_w = 0.0;
    const double telemetry_freeze_torque_range_nm = 0.0;
    std::vector<std::array<uint64_t, 6>> telemetry_freeze_suspicion_start_time;

    // Expected drivetrain response detector.
    const uint64_t drivetrain_under_response_grace_period_ms = 5000;
    const uint64_t drivetrain_aerodynamic_collapse_grace_period_ms = 1500;
    const double drivetrain_min_expected_power_w = 5.0e5;
    const double drivetrain_min_aerodynamic_power_w = 2.0e5;
    const double drivetrain_rpm_low_fraction = 0.70;
    const double drivetrain_torque_low_fraction = 0.60;
    const double drivetrain_collapsed_rpm_fraction = 0.15;
    const double drivetrain_collapsed_torque_fraction = 0.15;
    const double drivetrain_collapsed_torque_nm = 200.0;
    std::vector<uint64_t> drivetrain_under_response_start_time;

    // Static telemetry bounds detector for obviously implausible received values.
    const uint64_t static_bounds_measurement_timeout_ms = 3000;
    const double static_bounds_wind_speed_max_ms = 1.5 * GlobalData::cutOutWindSpeed;
    const double static_bounds_orientation_window_deg = 60.0;
    const double static_bounds_rpm_max = 1.5 * GlobalData::ratedRotorSpeed;
    const double static_bounds_power_min_w = -0.10 * GlobalData::ratedPower;
    const double static_bounds_power_max_w = 1.25 * GlobalData::ratedPower;
    const double static_bounds_torque_min_nm = -0.10 * GlobalData::maximumGeneratorTorque;
    const double static_bounds_torque_max_nm = 1.50 * GlobalData::maximumGeneratorTorque;

    // Fleet peer detector for one turbine behaving unlike its peers while all
    // turbines are operating.
    const uint64_t fleet_peer_outlier_grace_period_ms = 8000;
    const double fleet_peer_min_expected_power_w = 1.0e6;
    const double fleet_peer_power_ratio_threshold = 0.30;
    const double fleet_peer_rpm_ratio_threshold = 0.25;
    std::vector<uint64_t> fleet_peer_outlier_start_time;

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

    bool checkConsistencyTelemetryFreezeReplay();
    /**
     * @brief Checks for nonzero telemetry signals with too little variation.
     */

    bool checkConsistencyDrivetrainUnderResponse();
    /**
     * @brief Checks whether rotor speed or generator torque is too low for the
     * expected operating point implied by wind, yaw, power setpoint, and mode.
     */

    bool checkStaticTelemetryBounds();
    /**
     * @brief Checks received telemetry against static engineering bounds.
     */

    bool checkConsistencyFleetPeerOutlier();
    /**
     * @brief Checks whether one turbine is an outlier relative to the rest of
     * the operating fleet after wind/yaw/setpoint normalization.
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
