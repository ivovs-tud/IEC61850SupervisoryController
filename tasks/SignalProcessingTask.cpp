#include <vector>
#include <numeric>

#include "SignalProcessingTask.hpp"
#include "common/DataHistorian.hpp"
#include "common/GlobalDataStructure.hpp"
#include "common/TimeUtils.hpp"

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
    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
		auto& gds = GlobalDataStructure::instance().data();
		gds.Wtotal_meas.push_back(std::accumulate(gds._W.begin(), gds._W.end(), 0.0));
        gds.TotalPower_recv = 0;
        for (int i = 0; i < N_TURBINES; ++i) gds.TotalPower_recv += gds.lastPower[i];
    }

    // The global wind speed and direction is determined based on the average of all latest received data from each turbine
    float ws_sum = 0.0f;
    float wd_sum = 0.0f;
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        for (std::size_t i = 0; i < GlobalDataStructure::instance().data().lastWS.size(); ++i) {
            if (GlobalDataStructure::instance().data().lastWS[i] > 0.0f) { // Assuming a valid wind speed is always positive
                ws_sum += GlobalDataStructure::instance().data().lastWS[i];
                wd_sum += GlobalDataStructure::instance().data().lastWD[i];
                count++;
            }
        }
    }

    if (count > 0) {
        float avg_ws = ws_sum / count;
        float avg_wd = wd_sum / count;
        {
            std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
            GlobalDataStructure::instance().data().glob_ws_i = avg_ws;
            GlobalDataStructure::instance().data().glob_wd_i = avg_wd;
        }
    }
    std::string logMsg = "[SP]" + std::to_string(getCurrentTimeMs()) + ";GV=" + std::to_string(GlobalDataStructure::instance().data().glob_ws_i) + ";GD=" + std::to_string(GlobalDataStructure::instance().data().glob_wd_i);
    DataHistorian::instance().log(logMsg);
}
