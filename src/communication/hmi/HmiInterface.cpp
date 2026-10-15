#include "HmiInterface.hpp"

#include "sc/runtime/Logging.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <limits>
#include <msgpack.hpp>
#include <mutex>
#include <string>
#include <sys/stat.h>

// =============================================================================
// Default signal configuration
//
// Each HmiSignalDef describes one subplot:
//   - lineLabels gives a curve per turbine (or a global scalar with one label)
//   - accessor   reads from SharedData and returns one value per label entry
//
// Edit this function to add, remove, or reorder signal groups.
// =============================================================================
HmiData collectHmiData(const SharedData& data)
{
    HmiData result;
    {
        std::lock_guard<std::mutex> lock(data.collected.mutex);
        result.turbinePower = data.collected.lastPower;
        result.turbineYaw = data.collected.lastYawOffset;
        result.turbineWindSpeed = data.collected.lastWS;
        result.turbineWindDirection = data.collected.lastWD;
        result.turbineRotorSpeed = data.collected.lastRPM;
        result.turbineGeneratorTorque = data.collected.lastGenTorque;
    }
    {
        std::lock_guard<std::mutex> lock(data.processed.mutex);
        result.measuredTotalPower =
            data.processed.measuredTotalPowerHistory.empty() ? 0.0 : data.processed.measuredTotalPowerHistory.back();
        result.totalReceivedPower = data.processed.totalReceivedPower;
        result.farmWindSpeed = data.processed.windSpeed;
        result.farmWindDirection = data.processed.windDirection;
        result.connectedTurbines = data.processed.connectedTurbines;
    }
    {
        std::lock_guard<std::mutex> lock(data.control.mutex);
        result.powerSetpoints = data.control.powerSetpoints;
        result.yawSetpoints = data.control.yawSetpoints;
        result.requestedPower = data.control.requestedPower;
        result.operationMode = data.control.turbineController.empty() ? 0 : static_cast<int>(data.control.turbineController.front());
        result.yawSteeringEnabled = data.control.yawSteeringEnabled;
        result.yawSteeringCommandName = data.control.yawSteeringCommandName;
        result.turbineEnabled = data.control.turbineEnabled;
    }
    {
        std::lock_guard<std::mutex> lock(data.monitoring.mutex);
        result.alarmWRecMeas = data.monitoring.alarmWRecMeas;
        result.alarmOrientationMisalign = data.monitoring.alarmOrientationMisalign;
        result.alarmWTorqueRotSpd = data.monitoring.alarmWTorqueRotSpd;
        result.alarmPowerExpected = data.monitoring.alarmPowerExpected;
        result.alarmHorWdDir = data.monitoring.alarmHorWdDir;
        result.alarmHorWdDirChg = data.monitoring.alarmHorWdDirChg;
        result.alarmHorWdSpdChg = data.monitoring.alarmHorWdSpdChg;
        result.alarmTelemetryFreezeReplay = data.monitoring.alarmTelemetryFreezeReplay;
        result.alarmDrivetrainUnderResponse = data.monitoring.alarmDrivetrainUnderResponse;
        result.alarmStaticBounds = data.monitoring.alarmStaticBounds;
        result.alarmFleetPeerOutlier = data.monitoring.alarmFleetPeerOutlier;
    }
    {
        std::lock_guard<std::mutex> lock(data.interface.mutex);
        result.systemRunning = data.interface.systemRunning;
        result.attackTapEnabled = data.interface.attackTapEnabled;
        result.attackTapAvailable = data.interface.attackTapAvailable;
        result.attackFdiEnabled = data.interface.attackFdiEnabled;
        result.attackFdiAvailable = data.interface.attackFdiAvailable;
        result.attackFdiSignals = data.interface.attackFdiSignals;
    }
    return result;
}

HmiConfig defaultHmiConfig(int numTurbines)
{
    // Build {"T1", "T2", ..., "Tn"} labels
    auto turbineLabels = [numTurbines]() {
        std::vector<std::string> lbl;
        lbl.reserve(static_cast<std::size_t>(numTurbines));
        for (int i = 1; i <= numTurbines; ++i)
            lbl.push_back("T" + std::to_string(i));
        return lbl;
    };

    // Safe slice helper: returns min(numTurbines, vec.size()) elements
    auto safeSlice = [numTurbines](const std::vector<double>& v) {
        int n = std::min(numTurbines, static_cast<int>(v.size()));
        return std::vector<double>(v.begin(), v.begin() + n);
    };

    auto turbineLabelsWithGlobal = [turbineLabels]() {
        std::vector<std::string> labels = turbineLabels();
        labels.push_back("Global");
        return labels;
    };

    HmiConfig cfg;
    cfg.numTurbines = numTurbines;
    cfg.windowSize = DEFAULT_HMI_SIGNAL_WINDOW_SIZE;
#ifdef PLATFORM_WINDOWS
    cfg.publisherEndpoint = "tcp://127.0.0.1:5555";
    cfg.commandEndpoint = "tcp://127.0.0.1:5556";
#else
    cfg.publisherEndpoint = "ipc:///tmp/supervisory_controller_hmi.sock";
    cfg.commandEndpoint = "ipc:///tmp/supervisory_controller_hmi_cmd.sock";
#endif

    cfg.signals = {
        // ── Per-turbine measured power and setpoints in one subplot ──────────
        {"Turbine Power and Setpoints", "W",
         [numTurbines]() {
             std::vector<std::string> labels;
             labels.reserve(static_cast<std::size_t>(numTurbines * 2));
             for (int i = 1; i <= numTurbines; ++i) {
                 labels.push_back("T" + std::to_string(i) + " Power");
                 labels.push_back("T" + std::to_string(i) + " Setpoint");
             }
             return labels;
         }(),
         [numTurbines](const HmiData& data) {
             int n =
                 std::min(numTurbines, std::min(static_cast<int>(data.turbinePower.size()), static_cast<int>(data.powerSetpoints.size())));
             std::vector<double> v;
             v.reserve(static_cast<std::size_t>(n * 2));
             for (int i = 0; i < n; ++i) {
                 v.push_back(data.turbinePower[i]);
                 if (data.powerSetpoints[i] < 0.0f) {
                     // This means, maximize power generation -> We push back NaN to indicate this
                     v.push_back(std::numeric_limits<double>::quiet_NaN());
                 } else {
                     v.push_back(static_cast<double>(data.powerSetpoints[i]));
                 }
             }
             return v;
         },
         std::make_pair(-100000.0, 7e6)},
        // ── Per-turbine measured yaw offset and setpoints in one subplot ─────
        {"Turbine Orientation and Setpoints", "deg",
         [numTurbines]() {
             std::vector<std::string> labels;
             labels.reserve(static_cast<std::size_t>(numTurbines * 2));
             for (int i = 1; i <= numTurbines; ++i) {
                 labels.push_back("T" + std::to_string(i) + " Orientation");
             }
             for (int i = 1; i <= numTurbines; ++i) {
                 labels.push_back("T" + std::to_string(i) + " Orientation Setpoint");
             }
             return labels;
         }(),
         [numTurbines](const HmiData& data) {
             int n = std::min(numTurbines, std::min(static_cast<int>(data.turbineYaw.size()), static_cast<int>(data.yawSetpoints.size())));
             std::vector<double> v;
             v.reserve(static_cast<std::size_t>(n * 2));
             for (int i = 0; i < n; ++i) {
                 v.push_back(data.turbineYaw[i]);
             }
             for (int i = 0; i < n; ++i) {
                 v.push_back(static_cast<double>(data.yawSetpoints[i]));
             }
             return v;
         },
         std::make_pair(215.0, 325.0)},
        // ── Farm-level reference vs. total delivered power ────────────────────
        {"Farm Reference vs. Total Power",
         "W",
         {"Reference", "Total (Meas)", "Total (Received)"},
         [](const HmiData& data) { return std::vector<double>{data.requestedPower, data.measuredTotalPower, data.totalReceivedPower}; },
         std::make_pair(-1000000.0, static_cast<double>(numTurbines) * 7e6)},
        // ── Per-turbine wind speed ────────────────────────────────────────────
        {"Wind Speed", "m/s", turbineLabelsWithGlobal(),
         [safeSlice](const HmiData& data) {
             std::vector<double> v = safeSlice(data.turbineWindSpeed);
             v.push_back(data.farmWindSpeed);
             return v;
         },
         std::make_pair(-1.0, 22.0)},
        // ── Per-turbine wind direction ────────────────────────────────────────
        {"Wind Direction", "deg", turbineLabelsWithGlobal(),
         [safeSlice](const HmiData& data) {
             std::vector<double> v = safeSlice(data.turbineWindDirection);
             v.push_back(data.farmWindDirection);
             return v;
         },
         std::make_pair(-10.0, 360.0)},
        // ── Per-turbine rotor speed ───────────────────────────────────────────
        {"Rotor Speed", "RPM", turbineLabels(), [safeSlice](const HmiData& data) { return safeSlice(data.turbineRotorSpeed); },
         std::make_pair(-1, 20)},
        // -- Per-turbine generator torque ------------------------------------
        {"Generator Torque", "Nm", turbineLabels(), [safeSlice](const HmiData& data) { return safeSlice(data.turbineGeneratorTorque); },
         std::make_pair(-1000.0, 5e4)}};

    return cfg;
}

// =============================================================================
// HmiInterface implementation
// =============================================================================
HmiInterface::HmiInterface(HmiConfig config, std::chrono::milliseconds period) : PeriodicTask(period), config_(std::move(config))
{
}

HmiInterface::~HmiInterface()
{
    stop();
}

void HmiInterface::onStart()
{
    try {
        std::string pubIpcPath;
        if (config_.publisherEndpoint.rfind("ipc://", 0) == 0) {
            pubIpcPath = config_.publisherEndpoint.substr(6);
            if (!pubIpcPath.empty()) {
                // Remove stale socket node left by previous crashes/runs.
                std::error_code ec;
                std::filesystem::remove(pubIpcPath, ec);
            }
        }

        std::string cmdIpcPath;
        if (config_.commandEndpoint.rfind("ipc://", 0) == 0) {
            cmdIpcPath = config_.commandEndpoint.substr(6);
            if (!cmdIpcPath.empty()) {
                std::error_code ec;
                std::filesystem::remove(cmdIpcPath, ec);
            }
        }

        pubSocket_.emplace(context_, zmq::socket_type::pub);
        // Drop oldest frame if the subscriber falls behind rather than blocking.
        pubSocket_->set(zmq::sockopt::sndhwm, 5);
        pubSocket_->bind(config_.publisherEndpoint);

        cmdSocket_.emplace(context_, zmq::socket_type::pull);
        cmdSocket_->set(zmq::sockopt::rcvhwm, 10);
        cmdSocket_->bind(config_.commandEndpoint);
#ifdef PLATFORM_POSIX
        if (!pubIpcPath.empty()) {
            // Allow subscribers running as a different user (e.g., non-sudo HMI).
            if (::chmod(pubIpcPath.c_str(), 0666) != 0) {
                std::cerr << "[HmiInterface] Warning: failed to chmod IPC socket " << pubIpcPath << '\n';
            }
        }

        if (!cmdIpcPath.empty()) {
            if (::chmod(cmdIpcPath.c_str(), 0666) != 0) {
                std::cerr << "[HmiInterface] Warning: failed to chmod command IPC socket " << cmdIpcPath << '\n';
            }
        }
#endif
        std::cout << "[HmiInterface] Publishing on " << config_.publisherEndpoint << " | Command input on " << config_.commandEndpoint
                  << '\n';
    } catch (const std::exception& e) {
        std::cerr << "[HmiInterface] Failed to bind publisher: " << e.what() << '\n';
        pubSocket_.reset();
        cmdSocket_.reset();
        throw;
    }
}

void HmiInterface::onStop()
{
    pubSocket_.reset();
    cmdSocket_.reset();
}

void HmiInterface::handleCommands()
{
    if (!cmdSocket_)
        return;

    while (true) {
        zmq::message_t cmdMsg;
        const auto result = cmdSocket_->recv(cmdMsg, zmq::recv_flags::dontwait);
        if (!result)
            break;

        try {
            const auto* data = static_cast<const char*>(cmdMsg.data());
            msgpack::object_handle oh = msgpack::unpack(data, cmdMsg.size());
            msgpack::object obj = oh.get();

            if (obj.type != msgpack::type::ARRAY || obj.via.array.size < 2) {
                continue;
            }

            const std::string cmd = obj.via.array.ptr[0].as<std::string>();
            if (cmd == "set_mode") {
                int requestedMode = obj.via.array.ptr[1].as<int>();
                requestedMode = std::max(0, std::min(2, requestedMode));

                SharedData& d = SharedData::instance();
                std::lock_guard<std::mutex> lock(d.control.mutex);
                std::fill(d.control.turbineController.begin(), d.control.turbineController.end(), requestedMode + 1);

                if (requestedMode == 0)
                    d.control.statusMessage = "Mode: ROSCO";
                if (requestedMode == 1)
                    d.control.statusMessage = "Mode: Lio-Downregulation";
                if (requestedMode == 2)
                    d.control.statusMessage = "Mode: Safe Shutdown";
            } else if (cmd == "set_button_state") {
                if (obj.via.array.size < 3)
                    continue;
                std::string buttonName = obj.via.array.ptr[1].as<std::string>();
                int buttonState = obj.via.array.ptr[2].as<int>();

                SharedData& d = SharedData::instance();
                std::lock_guard<std::mutex> lock(d.control.mutex);

                if (buttonName == "Yaw Steering") {
                    d.control.yawSteeringEnabled = (buttonState != 0);
                    d.control.statusMessage = std::string("Yaw Steering: ") + (d.control.yawSteeringEnabled ? "On" : "Off");
                } else if (buttonName == "Enable Turbines") {
                    uint32_t enableValue = (buttonState != 0) ? 1 : 0;
                    std::fill(d.control.turbineEnabled.begin(), d.control.turbineEnabled.end(), enableValue);
                    d.control.statusMessage = std::string("Enable Turbines: ") + (buttonState != 0 ? "On" : "Off");
                }
            } else if (cmd == "set_turbine_enable") {
                if (obj.via.array.size < 3)
                    continue;
                int turbineId = obj.via.array.ptr[1].as<int>();
                int enabled = obj.via.array.ptr[2].as<int>();

                SharedData& d = SharedData::instance();
                std::lock_guard<std::mutex> lock(d.control.mutex);
                if (turbineId < 1 || turbineId > static_cast<int>(d.control.turbineEnabled.size())) {
                    continue;
                }

                d.control.turbineEnabled[static_cast<std::size_t>(turbineId - 1)] = (enabled != 0) ? 1U : 0U;
                d.control.statusMessage =
                    "T" + std::to_string(turbineId) + std::string(" turbine: ") + (enabled != 0 ? "Enabled" : "Disabled");
            } else if (cmd == "acknowledge_alarms" && config_.alarmAcknowledgementEnabled) {
                SharedData& d = SharedData::instance();
                std::lock_guard<std::mutex> lock(d.control.mutex);
                d.control.alarmAcknowledgementRequested = true;
                d.control.statusMessage = "Alarms acknowledged";
            }
        } catch (const std::exception&) {
            // Ignore malformed commands and continue.
        }
    }
}

void HmiInterface::execute()
{
    handleCommands();

    const HmiData data = collectHmiData(SharedData::instance());
    std::vector<std::vector<double>> snap(config_.signals.size());
    for (std::size_t i = 0; i < config_.signals.size(); ++i)
        snap[i] = config_.signals[i].accessor(data);

    ++tickCount_;

    if (!pubSocket_)
        return;

    // ── 2. Pack snapshot as msgpack and publish ───────────────────────────────
    // Format:
    // [tick, window_size, [[name, unit, [labels], [values], [y_min, y_max]|nil], ...],
    //  [[light_name, is_on, color], ...], [operation_mode, [mode_labels...]]]
    // The Python subscriber maintains the rolling window; we send only the
    // latest values each cycle.
    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(buf);

    pk.pack_array(config_.alarmAcknowledgementEnabled ? 10 : 9);
    pk.pack(tickCount_);
    pk.pack(static_cast<int32_t>(config_.windowSize));

    pk.pack_array(config_.signals.size());
    for (std::size_t i = 0; i < config_.signals.size(); ++i) {
        const auto& sig = config_.signals[i];
        pk.pack_array(5);
        pk.pack(sig.name);
        pk.pack(sig.unit);
        pk.pack(sig.lineLabels);
        pk.pack(snap[i]);
        if (sig.defaultYRange) {
            pk.pack_array(2);
            pk.pack(sig.defaultYRange->first);
            pk.pack(sig.defaultYRange->second);
        } else {
            pk.pack_nil();
        }
    }

    const std::array<const char*, 3> modeLabels{{"Kω²", "Down-\nregulation", "Shutdown"}};
    const char* systemRunningColor = "red";
    if (data.connectedTurbines >= config_.numTurbines && config_.numTurbines > 0) {
        systemRunningColor = "green";
    } else if (data.connectedTurbines > 0) {
        systemRunningColor = "amber";
    }

    pk.pack_array(12);
    pk.pack_array(3);
    pk.pack("System Running");
    pk.pack(data.systemRunning);
    pk.pack(systemRunningColor);
    pk.pack_array(3);
    pk.pack("Prec != Pmeas");
    pk.pack(data.alarmWRecMeas);
    pk.pack("red");
    pk.pack_array(3);
    pk.pack("Pmeas != Pexpected");
    pk.pack(data.alarmPowerExpected);
    pk.pack("red");
    pk.pack_array(3);
    pk.pack("Orientation");
    pk.pack(data.alarmOrientationMisalign);
    pk.pack("red");
    pk.pack_array(3);
    pk.pack("P != ω * Tgen");
    pk.pack(data.alarmWTorqueRotSpd);
    pk.pack("red");
    pk.pack_array(3);
    pk.pack("WD Consistency");
    pk.pack(data.alarmHorWdDir);
    pk.pack("red");
    pk.pack_array(3);
    pk.pack("WD Change");
    pk.pack(data.alarmHorWdDirChg);
    pk.pack("red");
    pk.pack_array(3);
    pk.pack("WS Change");
    pk.pack(data.alarmHorWdSpdChg);
    pk.pack("red");
    pk.pack_array(3);
    pk.pack("Telemetry Freeze");
    pk.pack(data.alarmTelemetryFreezeReplay);
    pk.pack("red");
    pk.pack_array(3);
    pk.pack("Small ω/Tgen");
    pk.pack(data.alarmDrivetrainUnderResponse);
    pk.pack("red");
    pk.pack_array(3);
    pk.pack("Static Bounds");
    pk.pack(data.alarmStaticBounds);
    pk.pack("red");
    pk.pack_array(3);
    pk.pack("Outliers");
    pk.pack(data.alarmFleetPeerOutlier);
    pk.pack("red");

    pk.pack_array(2);
    pk.pack(data.operationMode - 1);
    pk.pack(modeLabels);

    pk.pack_array(2);
    pk.pack("sample_period_ms");
    pk.pack(static_cast<int>(period_.count()));

    pk.pack_array(4);
    pk.pack(static_cast<int>(data.yawSteeringEnabled));
    pk.pack(data.yawSteeringCommandName);
    pk.pack("Yaw\nSteering Off");
    pk.pack("Yaw\nSteering On");

    pk.pack_array(2);
    pk.pack("turbine_enable_states");
    pk.pack(data.turbineEnabled);

    pk.pack_array(6);
    pk.pack("attack_resources");
    pk.pack(data.attackTapEnabled);
    pk.pack(data.attackTapAvailable);
    pk.pack(data.attackFdiEnabled);
    pk.pack(data.attackFdiAvailable);
    pk.pack(data.attackFdiSignals);

    if (config_.alarmAcknowledgementEnabled) {
        pk.pack_array(2);
        pk.pack("alarm_acknowledgement");
        pk.pack(true);
    }

    zmq::message_t msg(buf.data(), buf.size());
    pubSocket_->send(msg, zmq::send_flags::dontwait);
}

// =============================================================================
// HmiInterface implementation
// =============================================================================
