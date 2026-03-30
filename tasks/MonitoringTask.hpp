#pragma once

#include "common/PeriodicTask.hpp"

// ---------------------------------------------------------------------------
// MonitoringTask – system health monitoring + GOOSE subscriber.
// ---------------------------------------------------------------------------
class MonitoringTask : public PeriodicTask
{
public:
    explicit MonitoringTask(std::chrono::milliseconds period = std::chrono::milliseconds(50));

protected:
    void execute() override;

private:
    // GOOSE message callback placeholder.
    // TODO: replace void* params with GooseSubscriber* / GooseMessage* from libiec61850
    //   once libiec_wrapper exposes the subscription API.
    static void onGooseMessage(void* subscriber, void* parameter);
};
