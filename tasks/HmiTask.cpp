#include "HmiTask.hpp"
#include "common/config.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>

#include <sys/stat.h>

#include <msgpack.hpp>

// =============================================================================
// Default signal configuration
//
// Each HmiSignalDef describes one subplot:
//   - lineLabels gives a curve per turbine (or a global scalar with one label)
//   - accessor   reads from GlobalData and returns one value per label entry
//
// Edit this function to add, remove, or reorder signal groups.
// =============================================================================
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

    HmiConfig cfg;
    cfg.numTurbines        = numTurbines;
    cfg.windowSize         = 100;   // last 100 samples (= 10 s at 100 ms period)
#ifdef PLATFORM_WINDOWS
        cfg.publisherEndpoint  = "tcp://localhost:5555";
        cfg.commandEndpoint = "tcp://localhost:5556";
#else
        cfg.publisherEndpoint = "ipc:///tmp/supervisory_controller_hmi.sock";   
        cfg.commandEndpoint = "ipc:///tmp/supervisory_controller_hmi_cmd.sock";
#endif  

    cfg.signals = {
        // ── Per-turbine measured power and setpoints in one subplot ──────────
        {
            "Turbine Power and Setpoints", "W",
            [numTurbines]() {
                std::vector<std::string> labels;
                labels.reserve(static_cast<std::size_t>(numTurbines * 2));
                for (int i = 1; i <= numTurbines; ++i) {
                    labels.push_back("T" + std::to_string(i) + " Power");
                    labels.push_back("T" + std::to_string(i) + " Setpoint");
                }
                return labels;
            }(),
            [numTurbines](const GlobalData& d) {
                int n = std::min(numTurbines,
                                 std::min(static_cast<int>(d.lastPower.size()),
                                          static_cast<int>(d.TurbinePowerSetpoints.size())));
                std::vector<double> v;
                v.reserve(static_cast<std::size_t>(n * 2));
                for (int i = 0; i < n; ++i) {
                    v.push_back(d.lastPower[i]);
                    if (d.TurbinePowerSetpoints[i] < 0.0f) {
						// This means, maximize power generation -> We push back NaN to indicate this 
						v.push_back(std::numeric_limits<double>::quiet_NaN());
                    } else {
                        v.push_back(static_cast<double>(d.TurbinePowerSetpoints[i]));
                    }
                }
                return v;
            }
        },
        // ── Per-turbine measured yaw offset and setpoints in one subplot ─────
        {
            "Yaw Offset and Setpoints", "deg",
            [numTurbines]() {
                std::vector<std::string> labels;
                labels.reserve(static_cast<std::size_t>(numTurbines * 2));
                for (int i = 1; i <= numTurbines; ++i) {
                    labels.push_back("T" + std::to_string(i) + " Yaw Offset");
                    labels.push_back("T" + std::to_string(i) + " Yaw Setpoint");
                }
                return labels;
            }(),
            [numTurbines](const GlobalData& d) {
                int n = std::min(numTurbines,
                                 std::min(static_cast<int>(d.lastYawOffset.size()),
                                          static_cast<int>(d.TurbineYawSetpoints.size())));
                std::vector<double> v;
                v.reserve(static_cast<std::size_t>(n * 2));
                for (int i = 0; i < n; ++i) {
                    v.push_back(d.lastYawOffset[i]);
                    v.push_back(static_cast<double>(d.TurbineYawSetpoints[i]));
                }
                return v;
            }
        },
        // ── Farm-level reference vs. total delivered power ────────────────────
        {
            "Farm Reference vs. Total Power", "W",
            {"Reference", "Total (Meas)", "Total (Received)"},
            [numTurbines](const GlobalData& d) {
                double total = 0.0;
                int n = std::min(numTurbines, static_cast<int>(d.Power_i.size()));
                //for (int i = 0; i < n; ++i) total += d.Power_i[i];
                return std::vector<double>{
					static_cast<double>(d.RequestedReferencePower), d.Wtotal_meas.back(), d.TotalPower_recv
                };
            }
        },
        // ── Per-turbine wind speed ────────────────────────────────────────────
        {
            "Wind Speed", "m/s",
            turbineLabels(),
            [safeSlice](const GlobalData& d) { return safeSlice(d.lastWS); }
        },
        // ── Per-turbine wind direction ────────────────────────────────────────
        {
            "Wind Direction", "deg",
            turbineLabels(),
            [safeSlice](const GlobalData& d) { return safeSlice(d.lastWD); }
        },
        // ── Per-turbine rotor speed ───────────────────────────────────────────
        {
            "Rotor Speed", "RPM",
            turbineLabels(),
            [safeSlice](const GlobalData& d) { return safeSlice(d.lastRPM); }
        },
    };

    return cfg;
}

// =============================================================================
// HmiTask implementation
// =============================================================================
HmiTask::HmiTask(HmiConfig config, std::chrono::milliseconds period)
    : PeriodicTask(period)
    , config_(std::move(config))
{}

void HmiTask::onStart()
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
                std::cerr << "[HmiTask] Warning: failed to chmod IPC socket " << pubIpcPath << '\n';
            }
        }

        if (!cmdIpcPath.empty()) {
            if (::chmod(cmdIpcPath.c_str(), 0666) != 0) {
                std::cerr << "[HmiTask] Warning: failed to chmod command IPC socket " << cmdIpcPath << '\n';
            }
        }
#endif
        std::cout << "[HmiTask] Publishing on " << config_.publisherEndpoint
                  << " | Command input on " << config_.commandEndpoint << '\n';
    } catch (const std::exception& e) {
        std::cerr << "[HmiTask] Failed to bind publisher: " << e.what() << '\n';
        pubSocket_.reset();
        cmdSocket_.reset();
    }
}

void HmiTask::onStop()
{
    pubSocket_.reset();
    cmdSocket_.reset();
}

void HmiTask::handleCommands()
{
    if (!cmdSocket_) return;

    while (true) {
        zmq::message_t cmdMsg;
        const auto result = cmdSocket_->recv(cmdMsg, zmq::recv_flags::dontwait);
        if (!result) break;

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

                std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
                GlobalData& d = GlobalDataStructure::instance().data();
                d.operationMode = requestedMode;

                if (requestedMode == 0) d.statusMessage = "Mode: ROSCO";
                if (requestedMode == 1) d.statusMessage = "Mode: Lio-Downregulation";
                if (requestedMode == 2) d.statusMessage = "Mode: Safe Shutdown";
            }
        } catch (const std::exception&) {
            // Ignore malformed commands and continue.
        }
    }
}

void HmiTask::execute()
{
    handleCommands();

    // ── 1. Sample current values from shared state ────────────────────────────
    std::vector<std::vector<double>> snap(config_.signals.size());
    int operationMode = 0;
    bool alarmWRecMeas = false;
    bool alarmOrientationMisalign = false;
    bool alarmWTorqueRotSpd = false;
    bool alarmHorWdDir = false;
    bool systemRunning = false;
    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        const GlobalData& d = GlobalDataStructure::instance().data();
        for (std::size_t i = 0; i < config_.signals.size(); ++i)
            snap[i] = config_.signals[i].accessor(d);

        operationMode = d.operationMode;
        alarmWRecMeas = d.alarmWRecMeas;
        alarmOrientationMisalign = d.alarmOrientationMisalign;
        alarmWTorqueRotSpd = d.alarmWTorqueRotSpd;
        alarmHorWdDir = d.alarmHorWdDir;
        systemRunning = d.systemRunning;
    }

    ++tickCount_;

    if (!pubSocket_) return;

    // ── 2. Pack snapshot as msgpack and publish ───────────────────────────────
    // Format:
    // [tick, window_size, [[name, unit, [labels], [values]], ...],
    //  [[light_name, is_on, color], ...], [operation_mode, [mode_labels...]]]
    // The Python subscriber maintains the rolling window; we send only the
    // latest values each cycle.
    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(buf);

    pk.pack_array(5);
    pk.pack(tickCount_);
    pk.pack(static_cast<int32_t>(config_.windowSize));

    pk.pack_array(config_.signals.size());
    for (std::size_t i = 0; i < config_.signals.size(); ++i) {
        const auto& sig = config_.signals[i];
        pk.pack_array(4);
        pk.pack(sig.name);
        pk.pack(sig.unit);
        pk.pack(sig.lineLabels);
        pk.pack(snap[i]);
    }

    const std::array<const char*, 3> modeLabels{{"ROSCO", "Lio\nDownregulation", "Safe\nShutdown"}};

    pk.pack_array(5);
    pk.pack_array(3); pk.pack("System Running");    pk.pack(systemRunning);      pk.pack("green");
    pk.pack_array(3); pk.pack("Power Tracking");    pk.pack(alarmWRecMeas); pk.pack("red");
    pk.pack_array(3); pk.pack("Yaw Misalignment");  pk.pack(alarmOrientationMisalign); pk.pack("amber");
    pk.pack_array(3); pk.pack("Communication");     pk.pack(alarmWTorqueRotSpd);  pk.pack("red");
    pk.pack_array(3); pk.pack("Emergency Stop");    pk.pack(alarmHorWdDir);  pk.pack("red");

    pk.pack_array(2);
    pk.pack(operationMode);
    pk.pack(modeLabels);

    zmq::message_t msg(buf.data(), buf.size());
    pubSocket_->send(msg, zmq::send_flags::dontwait);
}


// =============================================================================
// HmiTask implementation
// =============================================================================


