#include <vector>
#include <numeric>
#include <algorithm>
#include <array>

#include "SignalProcessingTask.hpp"
#include "common/DataHistorian.hpp"
#include "common/GlobalDataStructure.hpp"
#include "common/util.hpp"

namespace {
constexpr uint64_t TURBINE_CONNECTION_TIMEOUT_MS = 2000;

bool hasRecentMeasurement(const GlobalData& gds, int turbineIndex, uint64_t nowMs)
{
    const std::array<uint64_t, 6> timestamps {
        gds.lastWS_t[turbineIndex],
        gds.lastWD_t[turbineIndex],
        gds.lastYawOffset_t[turbineIndex],
        gds.lastRPM_t[turbineIndex],
        gds.lastPower_t[turbineIndex],
        gds.lastGenTorque_t[turbineIndex],
    };

    return std::any_of(timestamps.begin(), timestamps.end(), [nowMs](uint64_t timestamp) {
        return timestamp != 0 && timestamp + TURBINE_CONNECTION_TIMEOUT_MS >= nowMs;
    });
}
}

SignalProcessingTask::SignalProcessingTask(std::chrono::milliseconds period)
    : PeriodicTask(period)
{
    // TODO: configure signal processing pipeline (filters, scaling)
}

void SignalProcessingTask::init()
{
    // TODO: initialise signal processing resources
}

void SignalProcessingTask::execute()
{
    // TODO: implement signal acquisition and processing
    // Example:
    //   std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
    //   GlobalDataStructure::instance().data().measuredVoltage = readAdc();
    const uint64_t nowMs = getCurrentTimeMs();
    float loggedWs = 0.0f;
    float loggedWd = 0.0f;
    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        auto& gds = GlobalDataStructure::instance().data();
		
		gds.Wtotal_meas.push_back(std::accumulate(gds._W.begin(), gds._W.end(), 0.0));
        gds.TotalPower_recv = 0;
        for (int i = 0; i < N_TURBINES; ++i) gds.TotalPower_recv += gds.lastPower[i];

        int connectedTurbines = 0;
        for (int i = 0; i < N_TURBINES; ++i) {
            if (hasRecentMeasurement(gds, i, nowMs)) {
                ++connectedTurbines;
            }
        }
        gds.connectedTurbines = connectedTurbines;
    }

    // The global wind speed and direction is determined based on the average of all latest received data from each turbine
    float wd_sum = 0.0f;
    float count = 0;
    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        auto& gds = GlobalDataStructure::instance().data();
        for (std::size_t i = 0; i < gds.lastWD.size(); ++i) {
            if (gds.lastWS[i] > 0.0f) { // Assuming a valid wind speed is always positive
                wd_sum += gds.lastWD[i];
                count++;
            }
        }
        if (count > 0) gds.glob_wd_i = static_cast<float>(wd_sum / count);
    }
    // For wind speed, we use the three biggest found items, and take their average
    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        auto& gds = GlobalDataStructure::instance().data();
        std::vector<float> tmp(3);
        std::partial_sort_copy(
            std::begin(gds.lastWS), std::end(gds.lastWS), //.begin/.end in C++98/C++03
            std::begin(tmp), std::end(tmp),
            std::greater<float>() //remove "int" in C++14
        );
        float res = std::accumulate(std::begin(tmp), std::end(tmp), 0.0f) / 3.0f;
        gds.glob_ws_i = res;
        loggedWs = gds.glob_ws_i;
        loggedWd = gds.glob_wd_i;
    }

    std::string logMsg = "[SP]" + std::to_string(getCurrentTimeMs()) + ";GV=" + std::to_string(loggedWs) + ";GD=" + std::to_string(loggedWd);
    DataHistorian::instance().log(logMsg);
}
