#include <vector>
#include <numeric>
#include <algorithm>
#include <array>

#include "SignalProcessingTask.hpp"
#include "common/DataHistorian.hpp"
#include "common/SharedData.hpp"
#include "common/util.hpp"

namespace {
constexpr uint64_t TURBINE_CONNECTION_TIMEOUT_MS = 2000;

bool hasRecentMeasurement(const CollectedData& data, std::size_t turbineIndex, uint64_t nowMs)
{
    const std::array<uint64_t, 6> timestamps {
        data.lastWS_t[turbineIndex],
        data.lastWD_t[turbineIndex],
        data.lastYawOffset_t[turbineIndex],
        data.lastRPM_t[turbineIndex],
        data.lastPower_t[turbineIndex],
        data.lastGenTorque_t[turbineIndex],
    };

    return std::any_of(timestamps.begin(), timestamps.end(), [nowMs](uint64_t timestamp) {
        return timestamp != 0 && timestamp + TURBINE_CONNECTION_TIMEOUT_MS >= nowMs;
    });
}

double calculateAvailablePower(double windSpeed)
{
    if (windSpeed < TurbineParameters::cutInWindSpeed || windSpeed >= TurbineParameters::cutOutWindSpeed) {
        return 0.0;
    }

    const double rotorRadius = TurbineParameters::rotorDiameter / 2.0;
    const double sweptArea = kPi * rotorRadius * rotorRadius;
    const double aerodynamicPower =
        0.5 * TurbineParameters::airDensity * sweptArea *
        TurbineParameters::optimalPowerCoefficient * windSpeed * windSpeed * windSpeed;

    return std::min(aerodynamicPower, TurbineParameters::ratedPower);
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
    const uint64_t nowMs = getCurrentTimeMs();
    float loggedWs = 0.0f;
    float loggedWd = 0.0f;
    auto& data = SharedData::instance();
    {
        std::scoped_lock lock(data.collected.mutex, data.processed.mutex);

        data.processed.measuredTotalPowerHistory.push_back(
            std::accumulate(data.collected.measuredPower.begin(), data.collected.measuredPower.end(), 0.0));
        data.processed.totalReceivedPower = std::accumulate(
            data.collected.lastPower.begin(), data.collected.lastPower.end(), 0.0);

        int connectedTurbines = 0;
        for (std::size_t i = 0; i < data.collected.lastPower.size(); ++i) {
            if (hasRecentMeasurement(data.collected, i, nowMs)) {
                ++connectedTurbines;
            }
            data.processed.availablePower[i] = calculateAvailablePower(data.collected.lastWS[i]);
        }
        data.processed.connectedTurbines = connectedTurbines;

        float wd_sum = 0.0f;
        float count = 0;
        for (std::size_t i = 0; i < data.collected.lastWD.size(); ++i) {
            if (data.collected.lastWS[i] > 0.0f) {
                wd_sum += data.collected.lastWD[i];
                count++;
            }
        }
        if (count > 0) {
            data.processed.windDirection =
                0.05f * (wd_sum / count) + 0.95f * data.processed.windDirection;
        }

        const std::size_t sampleCount = std::min<std::size_t>(3, data.collected.lastWS.size());
        std::vector<float> topWindSpeeds(sampleCount);
        std::partial_sort_copy(data.collected.lastWS.begin(), data.collected.lastWS.end(),
                               topWindSpeeds.begin(), topWindSpeeds.end(), std::greater<float>());
        const float windSpeed = std::accumulate(topWindSpeeds.begin(), topWindSpeeds.end(), 0.0f) /
                                static_cast<float>(sampleCount);
        data.processed.windSpeed = 0.9f * data.processed.windSpeed + 0.1f * windSpeed;
        loggedWs = data.processed.windSpeed;
        loggedWd = data.processed.windDirection;
    }

    std::string logMsg = "[SP]" + std::to_string(getCurrentTimeMs()) + ";GV=" + std::to_string(loggedWs) + ";GD=" + std::to_string(loggedWd);
    DataHistorian::instance().log(logMsg);
}
