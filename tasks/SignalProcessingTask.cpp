#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

#include "SignalProcessingTask.hpp"
#include "common/DataHistorian.hpp"
#include "common/SharedData.hpp"
#include "common/util.hpp"
#include "sc/application/SignalProcessing.hpp"

SignalProcessingTask::SignalProcessingTask(std::chrono::milliseconds period)
    : PeriodicTask(period) {}

void SignalProcessingTask::execute() {
    const uint64_t nowMs = getCurrentTimeMs();
    auto& data = SharedData::instance();

    sc::application::SignalProcessingInput input;
    input.currentTimeMs = nowMs;
    {
        std::lock_guard<std::mutex> lock(data.collected.mutex);
        input.turbines.reserve(data.collected.lastPower.size());
        for (std::size_t i = 0; i < data.collected.lastPower.size(); ++i) {
            const std::array<uint64_t, 6> timestamps{
                data.collected.lastWS_t[i],
                data.collected.lastWD_t[i],
                data.collected.lastYawOffset_t[i],
                data.collected.lastRPM_t[i],
                data.collected.lastPower_t[i],
                data.collected.lastGenTorque_t[i]};
            input.turbines.push_back({
                data.collected.lastWS[i],
                data.collected.lastWS_t[i],
                data.collected.lastWD[i],
                data.collected.lastWD_t[i],
                data.collected.lastPower[i],
                data.collected.measuredPower[i],
                *std::max_element(timestamps.begin(), timestamps.end())});
        }
    }

    {
        std::lock_guard<std::mutex> lock(data.processed.mutex);
        input.previousWindSpeed = data.processed.windSpeed;
        input.previousWindDirection = data.processed.windDirection;
    }

    const auto result = sc::application::processSignals(input);
    {
        std::lock_guard<std::mutex> lock(data.processed.mutex);
        if (result.availablePower.size() != data.processed.availablePower.size()) {
            throw std::logic_error("signal-processing result count does not match configured turbines");
        }
        data.processed.connectedTurbines = result.connectedTurbines;
        data.processed.availablePower = result.availablePower;
        data.processed.totalReceivedPower = result.totalReceivedPower;
        data.processed.measuredTotalPowerHistory.push_back(result.totalMeasuredPower);
        data.processed.windSpeed = result.windSpeed;
        data.processed.windDirection = result.windDirection;
    }

    const std::string logMsg = "[SP]" + std::to_string(getCurrentTimeMs()) +
                               ";GV=" + std::to_string(result.windSpeed) +
                               ";GD=" + std::to_string(result.windDirection);
    DataHistorian::instance().log(logMsg);
}
