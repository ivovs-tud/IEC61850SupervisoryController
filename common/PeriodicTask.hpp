#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// Thread-owning base for fixed-period tasks. start() waits until onStart()
// finishes, so a successful return means the worker is ready to execute.
class PeriodicTask
{
public:
    enum class State {
        Created,
        Starting,
        Running,
        StopRequested,
        Stopped,
        Failed,
    };

    using FailureHandler = std::function<void(const std::string&)>;

    explicit PeriodicTask(std::chrono::milliseconds period);
    virtual ~PeriodicTask();

    PeriodicTask(const PeriodicTask&) = delete;
    PeriodicTask& operator=(const PeriodicTask&) = delete;

    // Starts the worker and waits for onStart(). Returns false when startup
    // fails. Starting an already active or unjoined task is a logic error.
    bool start();
    void requestStop() noexcept;
    void waitStopped();
    void stop() noexcept;

    State state() const noexcept { return state_.load(); }
    bool isRunning() const noexcept { return state() == State::Running; }
    std::exception_ptr failure() const;
    std::string failureMessage() const;
    void setFailureHandler(FailureHandler handler);

protected:
    virtual void execute() = 0;
    virtual void onStart() {}
    virtual void onStop() {}

    std::chrono::milliseconds period_;
    static void SetThreadPriorityHelper();

private:
    void run();
    void recordFailure(std::exception_ptr failure) noexcept;
    static std::string describeFailure(const std::exception_ptr& failure) noexcept;

    std::atomic<bool> running_{false};
    std::atomic<State> state_{State::Created};
    std::thread thread_;
    mutable std::mutex lifecycleMutex_;
    std::condition_variable lifecycleCv_;
    std::condition_variable wakeCv_;
    std::exception_ptr failure_;
    FailureHandler failureHandler_;
};
