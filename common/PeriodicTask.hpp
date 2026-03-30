#pragma once

#include <atomic>
#include <chrono>
#include <thread>

// ---------------------------------------------------------------------------
// PeriodicTask – base class for all fixed-rate tasks.
//
// Owns its worker thread. Subclasses implement execute() for per-cycle work and
// optionally onStart() / onStop() for one-shot setup and teardown that run
// inside the worker thread before and after the loop.
//
// Usage:
//   class MyTask : public PeriodicTask {
//   public:
//       MyTask() : PeriodicTask(std::chrono::milliseconds(10)) {}
//   protected:
//       void execute() override { /* periodic work */ }
//   };
//
//   MyTask t;
//   t.start();   // spawns worker thread
//   ...
//   t.stop();    // signals loop to stop and joins thread
// ---------------------------------------------------------------------------
class PeriodicTask
{
public:
    explicit PeriodicTask(std::chrono::milliseconds period)
        : period_(period) {}

    virtual ~PeriodicTask() = default;

    PeriodicTask(const PeriodicTask&)            = delete;
    PeriodicTask& operator=(const PeriodicTask&) = delete;

    // Spawns the worker thread and begins periodic execution.
    void start()
    {
        running_.store(true);
        thread_ = std::thread([this]() {
            onStart();
            auto nextWakeup = std::chrono::steady_clock::now();
            while (running_.load()) {
                execute();
                nextWakeup += period_;
                std::this_thread::sleep_until(nextWakeup);
            }
            onStop();
        });
    }

    // Signals the loop to stop and blocks until the worker thread exits.
    void stop()
    {
        running_.store(false);
        if (thread_.joinable())
            thread_.join();
    }

protected:
    virtual void execute() = 0;  // called every period
    virtual void onStart() {}    // called once before the loop (in worker thread)
    virtual void onStop()  {}    // called once after  the loop (in worker thread)

private:
    std::chrono::milliseconds period_;
    std::atomic<bool>         running_{false};
    std::thread               thread_;
};