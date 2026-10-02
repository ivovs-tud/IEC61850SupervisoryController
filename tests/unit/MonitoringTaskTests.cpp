#include "sc/model/SharedData.hpp"
#include "sc/runtime/Time.hpp"
#include "sc/tasks/MonitoringTask.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>

namespace {

class TestMonitoringTask : public MonitoringTask {
    public:
    TestMonitoringTask() : MonitoringTask(std::chrono::milliseconds(50), 1, false)
    {
    }
    void runOnce()
    {
        execute();
    }
};

} // namespace

TEST_CASE("legacy alarm reset does not hide an alarm active in the current cycle")
{
    auto& data = SharedData::instance();
    data.configureTurbineCount(1);
    {
        std::lock_guard<std::mutex> lock(data.interface.mutex);
        data.interface.systemRunning = true;
    }
    {
        std::lock_guard<std::mutex> lock(data.collected.mutex);
        data.collected.lastWS[0] = 50.0;
        data.collected.lastWS_t[0] = getCurrentTimeMs();
    }
    {
        std::lock_guard<std::mutex> lock(data.monitoring.mutex);
        data.monitoring.alarmStaticBounds = false;
    }

    TestMonitoringTask task;
    task.runOnce();

    {
        std::lock_guard<std::mutex> lock(data.monitoring.mutex);
        REQUIRE(data.monitoring.alarmStaticBounds);
    }
    {
        std::lock_guard<std::mutex> lock(data.interface.mutex);
        data.interface.systemRunning = false;
    }
}
