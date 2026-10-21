#include "sc/model/SharedData.hpp"
#include "sc/runtime/DataHistorian.hpp"
#include "sc/runtime/Time.hpp"
#include "sc/tasks/MonitoringTask.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>

namespace {

class TestMonitoringTask : public MonitoringTask {
    public:
    explicit TestMonitoringTask(bool acknowledgement = false) : MonitoringTask(std::chrono::milliseconds(50), 1, acknowledgement)
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

    bool acknowledgement = false;
    SECTION("legacy alarms")
    {
    }
    SECTION("acknowledged alarms")
    {
        acknowledgement = true;
    }
    struct HistorianRun {
        std::filesystem::path directory =
            std::filesystem::temp_directory_path() / ("sc-monitoring-test-" + std::to_string(std::random_device{}()));
        ~HistorianRun()
        {
            DataHistorian::instance().stopRun();
            std::filesystem::remove_all(directory);
        }
    } run;
    auto& historian = DataHistorian::instance();
    historian.stopRun();
    historian.configure("monitoring", run.directory, 1);
    historian.start();
    TestMonitoringTask task(acknowledgement);
    task.runOnce();
    historian.flush();
    std::ifstream file(historian.currentFilePath());
    const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    REQUIRE(contents.find("[MON]") != std::string::npos);
    REQUIRE(contents.find(";StaticTelemetryBounds.Active=1;StaticTelemetryBounds.Visible=1") != std::string::npos);
    REQUIRE(contents.find(";TelemetryFreeze.Active=0") != std::string::npos);
    REQUIRE(contents.find("[MON_EVIDENCE]") != std::string::npos);
    REQUIRE(contents.find(";Detector=StaticTelemetryBounds;TurbineId=1") != std::string::npos);
    if (acknowledgement) {
        REQUIRE(contents.find(";StaticTelemetryBounds.Latched=1;StaticTelemetryBounds.Acknowledged=0") != std::string::npos);
    }

    {
        std::lock_guard<std::mutex> lock(data.monitoring.mutex);
        REQUIRE(data.monitoring.alarmStaticBounds);
    }
    {
        std::lock_guard<std::mutex> lock(data.interface.mutex);
        data.interface.systemRunning = false;
    }
}
