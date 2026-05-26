#pragma once

#include <cmath>

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
    /* TODO : Some of these are placeholders */
    const float orientation_time_constant = 10.0f; // Time constant for the orientation prediction model (in seconds). Depends on the turbine's yaw actuation dynamics.
    const float alpha_psi = exp(-period_.count() / 1000.0f / orientation_time_constant); // Constant defining the 'dynamics' of the orientation
    const float orientation_threshold = 5.0f; // Threshold for triggering the yaw misalignment alarm (in degrees)
    const float observer_gain = 0.5f; // Gain for a simple observer to estimate the true orientation based on measurements (placeholder value, needs tuning)
    // const uint64_t yaw_measurement_timeout_ms = 1000; // Time after which we consider the yaw measurement to be outdated (in milliseconds)
    std::vector<uint64_t> last_yaw_measurement_time; // Timestamp of the last yaw measurement that was used for detection
    std::vector<float> orientation_state;


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

    bool checkConsistencyOrientationDynamics();
    /**
        @brief Checks for consistency between where the turbine is expected to point and what is received

        In particular, the prediction model is 
        $$\psi_{k+1}^{(i)} = (1-\alpha_{\psi})\psi^{(i)}_{k} + \alpha_{\psi}\psi_{\mathrm{spt}}^{(i)}, \quad\text{ with }\, \alpha_{\psi} = \frac{\Delta t}{\tau_{\gamma} + \Delta t}.$$

        TODO: possibly upgrade this to an estimation/observer type of predictor
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
