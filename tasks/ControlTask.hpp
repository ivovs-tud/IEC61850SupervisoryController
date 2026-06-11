#pragma once

#include <string>
#include <vector>

#include "common/PeriodicTask.hpp"

/**
 * Periodic farm controller.
 *
 * Reads the operator power request and processed farm wind conditions, then
 * writes per-turbine power/yaw setpoints for IECCommunicator to transmit.
 */
class ControlTask : public PeriodicTask {
public:
    struct Config {
        int numTurbines;
        std::chrono::milliseconds period{std::chrono::milliseconds(10)};
        std::string yawLutCsvPath{"yaw_lut.csv"};
    };

    explicit ControlTask(Config config);

protected:
    void execute() override;
    void onStop()  override;  // stops socket servers after the loop exits

private:
    /**
     * Bilinear interpolation table for yaw steering offsets.
     *
     * CSV rows are keyed by wind-speed and wind-direction bins. Each row stores
     * one yaw offset per turbine; lookup returns interpolated offsets in the
     * same turbine order as the CSV columns.
     */
    class YawLut {
        using TurbineYawSetpoints = std::vector<float>;

    public:
        explicit YawLut(const std::string& csvFilePath);
        TurbineYawSetpoints lookup(float ws, float wd) const;

    private:
        std::vector<float> windSpeedBins_;
        std::vector<float> windDirectionBins_;
        std::vector<std::vector<TurbineYawSetpoints>> yawSetpoints_;
    };

    int numTurbines_;
    YawLut yawLut_;
};
