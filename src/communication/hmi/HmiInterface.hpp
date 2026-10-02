#pragma once

#include "sc/communication/hmi/HmiConfig.hpp"
#include "sc/runtime/PeriodicTask.hpp"

#include <optional>
#include <zmq.hpp>

class HmiInterface : public PeriodicTask {
    public:
    explicit HmiInterface(HmiConfig config = defaultHmiConfig(), std::chrono::milliseconds period = std::chrono::milliseconds(100));
    ~HmiInterface() override;

    protected:
    void onStart() override;
    void execute() override;
    void onStop() override;

    private:
    void handleCommands();

    HmiConfig config_;
    int64_t tickCount_ = 0;
    zmq::context_t context_;
    std::optional<zmq::socket_t> pubSocket_;
    std::optional<zmq::socket_t> cmdSocket_;
};
