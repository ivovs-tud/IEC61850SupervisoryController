#include "MonitoringTask.hpp"


MonitoringTask::MonitoringTask(std::chrono::milliseconds period)
    : PeriodicTask(period)
{
    // TODO: set up GOOSE subscriber via libiec_wrapper
}

void MonitoringTask::execute()
{
    // TODO: implement monitoring loop (thresholds, alarms, GOOSE events)
}

void MonitoringTask::onGooseMessage(void* subscriber, void* parameter)
{
    // TODO: handle incoming GOOSE message
    (void)subscriber;
    (void)parameter;
}
