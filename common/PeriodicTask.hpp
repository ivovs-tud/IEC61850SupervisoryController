#pragma once

#include <atomic>
#include <chrono>
#include <thread>
#include "common/config.hpp"

/**
 * Base class for fixed-rate worker threads.
 *
 * `onStart()` and `onStop()` run inside the worker thread, which makes them the
 * right place for thread-local setup/teardown such as binding sockets. `stop()`
 * is idempotent and joins the worker before returning.
 */
class PeriodicTask {
public:
    explicit PeriodicTask(std::chrono::milliseconds period)
        : period_(period) {}

    virtual ~PeriodicTask() = default;

    PeriodicTask(const PeriodicTask&)            = delete;
    PeriodicTask& operator=(const PeriodicTask&) = delete;


    void start();

    // Signals the loop to stop and blocks until the worker thread exits.
    void stop() {
        running_.store(false);
        if (thread_.joinable())
            thread_.join();
    }

protected:
    virtual void execute() = 0;
    virtual void onStart() {}
    virtual void onStop()  {}
    std::chrono::milliseconds period_;
    std::atomic<bool>         running_{false};
    std::thread               thread_;

    static void SetThreadPriorityHelper();
};
