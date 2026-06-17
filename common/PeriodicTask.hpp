#pragma once

#include <atomic>
#include <chrono>
#include <thread>
#include "common/config.hpp"



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


    void start();
    /**
	Start the periodic task by spawning a worker thread that executes the task's main loop. 
    The loop will call the execute() method at a fixed interval defined by the period_ member variable.
    The onStart() method is called once before entering the loop, 
    and onStop() is called once after exiting the loop. 
    The running_ atomic boolean is used to signal the loop to stop when needed.
    */

    // Signals the loop to stop without waiting for the worker thread.
    void requestStop()
    {
        running_.store(false);
    }

    // Blocks until the worker thread exits after a stop request.
    void waitStopped()
    {
        if (thread_.joinable())
            thread_.join();
    }

    // Signals the loop to stop and blocks until the worker thread exits.
    void stop()
    {
        requestStop();
        waitStopped();
    }

protected:
    virtual void execute() = 0;  // called every period
    virtual void onStart() {}    // called once before the loop (in worker thread)
    virtual void onStop()  {}    // called once after  the loop (in worker thread)
    std::chrono::milliseconds period_;
    std::atomic<bool>         running_{false};
    std::thread               thread_;

    static void SetThreadPriorityHelper();

private:

};
