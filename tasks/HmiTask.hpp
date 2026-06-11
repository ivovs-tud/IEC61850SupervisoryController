#pragma once

#include "common/GlobalDataStructure.hpp"
#include "common/PeriodicTask.hpp"

#include <functional>
#include <optional>
#include <utility>
#include <string>
#include <vector>

#include <zmq.hpp>

/**
 * One plot group in the external HMI.
 *
 * `accessor` is evaluated while GlobalData is locked, so it should copy only
 * the values needed for the current snapshot and avoid slow work.
 */
struct HmiSignalDef {
    std::string name;   // subplot title
    std::string unit;   // y-axis label
    std::vector<std::string> lineLabels;
    std::function<std::vector<double>(const GlobalData&)> accessor;
    std::optional<std::pair<double, double>> defaultYRange = std::nullopt;
};

/** HMI socket and signal configuration. */
struct HmiConfig {
    int numTurbines = 3;
    int windowSize  = 100;    // rolling sample count maintained by the Python plotter
    std::string publisherEndpoint = "ipc:///tmp/supervisory_controller_hmi.sock";
    std::string commandEndpoint   = "ipc:///tmp/supervisory_controller_hmi_cmd.sock";
    std::vector<HmiSignalDef> signals;
};

/// Build the default set of signal groups for @p numTurbines active turbines.
HmiConfig defaultHmiConfig(int numTurbines = 3);

/**
 * Publishes a msgpack HMI snapshot over ZeroMQ.
 *
 * Wire format:
 *   [tick, window_size, [[name, unit, [labels], [values], [y_min, y_max]|nil], ...],
 *    [[light_name, is_on, color], ...], [operation_mode, [mode_labels...]],
 *    [button_state, button_command]]
 *
 * The Python plotter owns rolling history; C++ sends only the current sample.
 */
class HmiTask : public PeriodicTask {
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
