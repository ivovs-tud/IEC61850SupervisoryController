#include "HmiTask.hpp"

#include <algorithm>
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
    cfg.publisherEndpoint  = "ipc:///tmp/supervisory_controller_hmi.sock";

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
                                 std::min(static_cast<int>(d.Power_i.size()),
                                          static_cast<int>(d.TurbinePowerSetpoints.size())));
                std::vector<double> v;
                v.reserve(static_cast<std::size_t>(n * 2));
                for (int i = 0; i < n; ++i) {
                    v.push_back(d.Power_i[i]);
                    v.push_back(static_cast<double>(d.TurbinePowerSetpoints[i]));
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
            {"Reference", "Total Delivered"},
            [numTurbines](const GlobalData& d) {
                double total = 0.0;
                int n = std::min(numTurbines, static_cast<int>(d.Power_i.size()));
                for (int i = 0; i < n; ++i) total += d.Power_i[i];
                return std::vector<double>{
                    static_cast<double>(d.RequestedReferencePower), total
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
        std::string ipcPath;
        if (config_.publisherEndpoint.rfind("ipc://", 0) == 0) {
            ipcPath = config_.publisherEndpoint.substr(6);
            if (!ipcPath.empty()) {
                // Remove stale socket node left by previous crashes/runs.
                std::error_code ec;
                std::filesystem::remove(ipcPath, ec);
            }
        }

        socket_.emplace(context_, zmq::socket_type::pub);
        // Drop oldest frame if the subscriber falls behind rather than blocking.
        socket_->set(zmq::sockopt::sndhwm, 5);
        socket_->bind(config_.publisherEndpoint);

        if (!ipcPath.empty()) {
            // Allow subscribers running as a different user (e.g., non-sudo HMI).
            if (::chmod(ipcPath.c_str(), 0666) != 0) {
                std::cerr << "[HmiTask] Warning: failed to chmod IPC socket " << ipcPath << '\n';
            }
        }

        std::cout << "[HmiTask] Publishing on " << config_.publisherEndpoint << '\n';
    } catch (const std::exception& e) {
        std::cerr << "[HmiTask] Failed to bind publisher: " << e.what() << '\n';
        socket_.reset();
    }
}

void HmiTask::onStop()
{
    socket_.reset();
}

void HmiTask::execute()
{
    // ── 1. Sample current values from shared state ────────────────────────────
    std::vector<std::vector<double>> snap(config_.signals.size());
    {
        std::lock_guard<std::mutex> lock(GlobalDataStructure::instance().mutex());
        const GlobalData& d = GlobalDataStructure::instance().data();
        for (std::size_t i = 0; i < config_.signals.size(); ++i)
            snap[i] = config_.signals[i].accessor(d);
    }

    ++tickCount_;

    if (!socket_) return;

    // ── 2. Pack snapshot as msgpack and publish ───────────────────────────────
    // Format: [tick, window_size, [[name, unit, [labels], [values]], ...]]
    // The Python subscriber maintains the rolling window; we send only the
    // latest values each cycle.
    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(buf);

    pk.pack_array(3);
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

    zmq::message_t msg(buf.data(), buf.size());
    socket_->send(msg, zmq::send_flags::dontwait);
}


// =============================================================================
// HmiTask implementation
// =============================================================================


