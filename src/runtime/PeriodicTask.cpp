#include "sc/runtime/PeriodicTask.hpp"

#include <stdexcept>
#include <utility>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

PeriodicTask::PeriodicTask(std::chrono::milliseconds period) : period_(period) {
    if (period_.count() <= 0) {
        throw std::invalid_argument("PeriodicTask period must be greater than zero");
    }
}

PeriodicTask::~PeriodicTask() {
    stop();
}

bool PeriodicTask::start() {
    std::unique_lock<std::mutex> lock(lifecycleMutex_);
    const State currentState = state_.load();
    if (thread_.joinable() || currentState == State::Starting ||
        currentState == State::Running || currentState == State::StopRequested) {
        throw std::logic_error("PeriodicTask is already active or has not been joined");
    }

    failure_ = nullptr;
    startupFinished_ = false;
    startupSucceeded_ = false;
    running_.store(true);
    state_.store(State::Starting);
    try {
        thread_ = std::thread(&PeriodicTask::run, this);
    } catch (...) {
        running_.store(false);
        state_.store(State::Failed);
        startupFinished_ = true;
        failure_ = std::current_exception();
        throw;
    }

    lifecycleCv_.wait(lock, [this]() {
        return startupFinished_;
    });
    const bool started = startupSucceeded_;
    lock.unlock();

    if (!started) {
        waitStopped();
    }
    return started;
}

void PeriodicTask::requestStop() noexcept {
    {
        // Update the wait predicate under its mutex to prevent a lost wake-up.
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        running_.store(false);
        State currentState = state_.load();
        while ((currentState == State::Starting || currentState == State::Running) &&
               !state_.compare_exchange_weak(currentState, State::StopRequested)) {
        }
    }
    lifecycleCv_.notify_all();
    wakeCv_.notify_all();
}

void PeriodicTask::waitStopped() {
    if (!thread_.joinable()) {
        return;
    }
    if (thread_.get_id() == std::this_thread::get_id()) {
        throw std::logic_error("PeriodicTask cannot join its own worker thread");
    }
    thread_.join();
}

void PeriodicTask::stop() noexcept {
    requestStop();
    try {
        waitStopped();
    } catch (...) {
        std::terminate();
    }
}

std::exception_ptr PeriodicTask::failure() const {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    return failure_;
}

std::string PeriodicTask::failureMessage() const {
    return describeFailure(failure());
}

void PeriodicTask::setFailureHandler(FailureHandler handler) {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    failureHandler_ = std::move(handler);
}

void PeriodicTask::run() {
    bool failed = false;
    try {
        onStart();
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            startupSucceeded_ = running_.load();
            startupFinished_ = true;
            state_.store(startupSucceeded_ ? State::Running : State::StopRequested);
        }
        lifecycleCv_.notify_all();

        SetThreadPriorityHelper();
        auto nextWakeup = std::chrono::steady_clock::now();
        while (running_.load()) {
            execute();
            nextWakeup += period_;
            std::unique_lock<std::mutex> lock(lifecycleMutex_);
            wakeCv_.wait_until(lock, nextWakeup, [this]() {
                return !running_.load();
            });
        }
    } catch (...) {
        failed = true;
        running_.store(false);
        recordFailure(std::current_exception());
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (!startupFinished_) {
            startupFinished_ = true;
            startupSucceeded_ = false;
        }
    }

    try {
        onStop();
    } catch (...) {
        if (!failed) {
            failed = true;
            recordFailure(std::current_exception());
        }
    }

    running_.store(false);
    state_.store(failed ? State::Failed : State::Stopped);
    lifecycleCv_.notify_all();
    wakeCv_.notify_all();

    FailureHandler handler;
    std::string message;
    if (failed) {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        handler = failureHandler_;
        message = describeFailure(failure_);
    }
    if (handler) {
        try {
            handler(message);
        } catch (...) {
            // Failure reporting must never terminate the worker thread.
        }
    }
}

void PeriodicTask::recordFailure(std::exception_ptr failure) noexcept {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (!failure_) {
        failure_ = std::move(failure);
    }
}

std::string PeriodicTask::describeFailure(const std::exception_ptr& failure) noexcept {
    if (!failure) {
        return {};
    }
    try {
        std::rethrow_exception(failure);
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "unknown task exception";
    }
}

void PeriodicTask::SetThreadPriorityHelper() {
#if defined(_WIN32) || defined(_WIN64)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
}
