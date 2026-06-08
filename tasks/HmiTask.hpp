#pragma once

#include "common/GlobalDataStructure.hpp"
#include "common/PeriodicTask.hpp"

#include <functional>
#include <optional>
#include <utility>
#include <string>
#include <vector>

#include <zmq.hpp>

// ---------------------------------------------------------------------------
// HmiSignalDef – one subplot in the HMI display.
//
//   lineLabels : display name for each line drawn in the subplot, e.g.
//                {"T1","T2","T3"} for a per-turbine signal.
//   accessor   : called every cycle; must return one value per label entry.
//   defaultYRange : optional fixed y-axis range applied by the plotter.
// ---------------------------------------------------------------------------
struct HmiSignalDef
{
    std::string name;   // subplot title
    std::string unit;   // y-axis label (e.g. "W", "m/s", "RPM")
    std::vector<std::string> lineLabels;
    std::function<std::vector<double>(const GlobalData&)> accessor;
    std::optional<std::pair<double, double>> defaultYRange = std::nullopt;
};

// ---------------------------------------------------------------------------
// HmiConfig – top-level HMI configuration.
//
// Edit defaultHmiConfig() in HmiTask.cpp to add / remove signal groups.
// ---------------------------------------------------------------------------
struct HmiConfig
{
    int numTurbines = 3;                 ///< active turbines to track
    int windowSize  = 100;               ///< rolling window length forwarded to the plotter
    std::string publisherEndpoint = "ipc:///tmp/supervisory_controller_hmi.sock"; ///< ZMQ PUB endpoint
    std::string commandEndpoint   = "ipc:///tmp/supervisory_controller_hmi_cmd.sock"; ///< ZMQ PULL endpoint (mode commands)
    std::vector<HmiSignalDef> signals;
};

/// Build the default set of signal groups for @p numTurbines active turbines.
HmiConfig defaultHmiConfig(int numTurbines = 3);

// ---------------------------------------------------------------------------
// HmiTask – samples GlobalDataStructure each cycle and publishes a msgpack
// snapshot over a ZeroMQ PUB socket consumed by hmi_plot.py.
//
// Wire format (msgpack array):
//   [tick, window_size, [[name, unit, [labels], [values], [y_min, y_max]|nil], ...],
//    [[light_name, is_on, color], ...], [operation_mode, [mode_labels...]],
//    [button_state, button_command]]
//
// The Python plotter maintains its own rolling history; C++ only sends the
// latest snapshot each cycle (no history buffers needed here).
// ---------------------------------------------------------------------------
class HmiTask : public PeriodicTask
{
public:
    explicit HmiTask(HmiConfig                  config = defaultHmiConfig(),
                     std::chrono::milliseconds  period = std::chrono::milliseconds(100));

protected:
    void onStart() override;
    void execute() override;
    void onStop()  override;

private:
    void handleCommands();

    HmiConfig                    config_;
    int64_t                      tickCount_ = 0;
    zmq::context_t               context_;
    std::optional<zmq::socket_t> pubSocket_;
    std::optional<zmq::socket_t> cmdSocket_;
};
