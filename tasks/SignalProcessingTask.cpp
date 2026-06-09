#include <vector>
#include <numeric>
#include <algorithm>

#include "SignalProcessingTask.hpp"
#include "common/DataHistorian.hpp"
#include "common/GlobalDataStructure.hpp"
#include "common/util.hpp"

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
    auto& gds = GlobalDataStructure::instance().data();
    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
		
		gds.Wtotal_meas.push_back(std::accumulate(gds._W.begin(), gds._W.end(), 0.0));
        gds.TotalPower_recv = 0;
        for (int i = 0; i < N_TURBINES; ++i) gds.TotalPower_recv += gds.lastPower[i];
    }

    // The global wind speed and direction is determined based on the average of all latest received data from each turbine
    float wd_sum = 0.0f;
    float count = 0;
    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
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
        std::vector<float> tmp(3);
        std::partial_sort_copy(
            std::begin(gds.lastWS), std::end(gds.lastWS), //.begin/.end in C++98/C++03
            std::begin(tmp), std::end(tmp),
            std::greater<float>() //remove "int" in C++14
        );
        float res = std::accumulate(std::begin(tmp), std::end(tmp), 0.0f) / 3.0f;
        gds.glob_ws_i = res;
    }

    std::string logMsg = "[SP]" + std::to_string(getCurrentTimeMs()) + ";GV=" + std::to_string(GlobalDataStructure::instance().data().glob_ws_i) + ";GD=" + std::to_string(GlobalDataStructure::instance().data().glob_wd_i);
    DataHistorian::instance().log(logMsg);
}
