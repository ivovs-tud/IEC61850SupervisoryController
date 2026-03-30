#include "HmiTask.hpp"


HmiTask::HmiTask(std::chrono::milliseconds period)
    : PeriodicTask(period)
{
    // TODO: initialise HMI connection / display resources
}

void HmiTask::execute()
{
    // TODO: implement HMI periodic update
}
