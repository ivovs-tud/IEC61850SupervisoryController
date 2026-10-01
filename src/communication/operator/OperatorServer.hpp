#pragma once

#include "OperatorCommand.hpp"
#include "sc/runtime/PeriodicTask.hpp"

#include <chrono>
#include <functional>
#include <optional>

#include <zmq.hpp>

class OperatorServer final : public PeriodicTask {
public:
    struct Config {
        int port{9001};
        std::chrono::milliseconds pollPeriod{10};
    };

    using CommandHandler = std::function<void(const OperatorCommand&)>;

    explicit OperatorServer(Config config);
    ~OperatorServer() override;

    void setCommandHandler(CommandHandler handler);

protected:
    void onStart() override;
    void execute() override;
    void onStop() override;

private:
    Config config_;
    zmq::context_t context_;
    std::optional<zmq::socket_t> socket_;
    CommandHandler commandHandler_;
};
