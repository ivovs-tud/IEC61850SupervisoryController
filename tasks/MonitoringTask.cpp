#include "MonitoringTask.hpp"
#include "common/GlobalDataStructure.hpp"



// ---------------------------------------------------------------------------
// Public Interface
// ---------------------------------------------------------------------------

MonitoringTask::MonitoringTask(std::chrono::milliseconds period)
    : PeriodicTask(period)
{
    // TODO: set up GOOSE subscriber via libiec_wrapper
}

void MonitoringTask::execute()
{
    // TODO: implement monitoring loop (thresholds, alarms, GOOSE events)
    auto& gds = GlobalDataStructure::instance().data();

    gds.alarmPowerTracking = checkConsistencyPowerGeneratedVsReceived();
}

void MonitoringTask::onGooseMessage(void* subscriber, void* parameter)
{
    // TODO: handle incoming GOOSE message
    (void)subscriber;
    (void)parameter;
}


// ---------------------------------------------------------------------------
// Private monitoring functions
// ---------------------------------------------------------------------------

bool MonitoringTask::checkConsistencyPowerGeneratedVsReceived() {
    //float total = 0;
    //std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
    auto& gds = GlobalDataStructure::instance().data();
    
    /*for (auto i = 0; i < gds.powerHistory.size(); i++) {
		total += gds.powerHistory[i].back();
    }*/
    
    return abs(gds.TotalPower_recv - gds.Wtotal_meas.back()) > 10e6;
}
