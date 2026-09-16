#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace sc::application {

class YawLut {
public:
    using TurbineYawSetpoints = std::vector<float>;

    explicit YawLut(const std::string& csvFilePath);

    TurbineYawSetpoints lookup(float windSpeed, float windDirection) const;
    std::size_t turbineCount() const noexcept;

private:
    std::vector<float> windSpeedBins_;
    std::vector<float> windDirectionBins_;
    std::vector<std::vector<TurbineYawSetpoints>> yawSetpoints_;
};

} // namespace sc::application
