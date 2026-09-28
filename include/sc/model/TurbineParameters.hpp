#pragma once

namespace sc {

// Default NREL 5 MW turbine and site values used by controller calculations.
struct TurbineParameters {
    static constexpr const char* modelName = "NREL5MW";
    static constexpr double airDensity = 1.225;
    static constexpr double rotorDiameter = 126.0;
    static constexpr double optimalPowerCoefficient = 0.482;
    static constexpr double optimalTipSpeedRatio = 7.55;
    static constexpr double rotorInertia = 4e6;
    static constexpr double gearboxRatio = 97.0;
    static constexpr double generatorEfficiency = 0.944;
    static constexpr double ratedPower = 5.8e6;
    static constexpr double cutInWindSpeed = 3.0;
    static constexpr double ratedWindSpeed = 11.4;
    static constexpr double cutOutWindSpeed = 25.0;
    static constexpr double ratedRotorSpeed = 12.1;
    static constexpr double minimumRotorSpeed = 0.722;
    static constexpr double pitchRate = 10.0;
    static constexpr double brakeTorque = 28116.2;
    static constexpr double yawingRate = 5.0;
    static constexpr double ratedTorque = 31465000.0;
    static constexpr double maximumGeneratorTorque = 47402.91;
};

} // namespace sc
