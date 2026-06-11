#include <vector>
#include <numeric>
#include <algorithm>

#include "SignalProcessingTask.hpp"
#include "common/DataHistorian.hpp"
#include "common/GlobalDataStructure.hpp"
#include "common/util.hpp"

SignalProcessingTask::SignalProcessingTask(std::chrono::milliseconds period)
    : PeriodicTask(period) {}

void SignalProcessingTask::init() {
}

void SignalProcessingTask::execute() {
    float farmWindSpeed = 0.0f;
    float farmWindDirection = 0.0f;

    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());

        auto& gds = GlobalDataStructure::instance().data();
        gds.measuredTotalPowerHistory.push_back(std::accumulate(gds.localMeasuredPower.begin(), gds.localMeasuredPower.end(), 0.0));
        gds.receivedTotalPower = 0;
        for (int i = 0; i < kMaxTurbines; ++i) {
            gds.receivedTotalPower += gds.lastPower[i];
        }

        float windDirectionSum = 0.0f;
        int sampleCount = 0;
        for (std::size_t i = 0; i < gds.lastWindDirection.size(); ++i) {
            if (gds.lastWindSpeed[i] > 0.0f) {
                windDirectionSum += gds.lastWindDirection[i];
                ++sampleCount;
            }
        }
        if (sampleCount > 0) {
            gds.farmWindDirection = static_cast<float>(windDirectionSum / sampleCount);
        }

        std::vector<float> topWindSpeeds(3);
        std::partial_sort_copy(
            std::begin(gds.lastWindSpeed), std::end(gds.lastWindSpeed),
            std::begin(topWindSpeeds), std::end(topWindSpeeds),
            std::greater<float>()
        );
        gds.farmWindSpeed = std::accumulate(std::begin(topWindSpeeds), std::end(topWindSpeeds), 0.0f) / 3.0f;
        farmWindSpeed = gds.farmWindSpeed;
        farmWindDirection = gds.farmWindDirection;
    }

    std::string logMsg = "[SP]" + std::to_string(getCurrentTimeMs()) + ";GV=" + std::to_string(farmWindSpeed) + ";GD=" + std::to_string(farmWindDirection);
    DataHistorian::instance().log(logMsg);
}
