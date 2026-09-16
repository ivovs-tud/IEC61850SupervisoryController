#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

#include "common/PeriodicTask.hpp"

using namespace std::chrono_literals;

namespace {

class CountingTask : public PeriodicTask {
public:
    explicit CountingTask(std::chrono::milliseconds period) : PeriodicTask(period) {}
    std::atomic<int> executions{0};

protected:
    void execute() override { executions.fetch_add(1); }
};

class StartAwareTask : public PeriodicTask {
public:
    StartAwareTask() : PeriodicTask(10ms) {}
    std::atomic<bool> ready{false};

protected:
    void onStart() override {
        std::this_thread::sleep_for(20ms);
        ready.store(true);
    }
    void execute() override {}
};

class FailingStartTask : public PeriodicTask {
public:
    FailingStartTask() : PeriodicTask(10ms) {}

protected:
    void onStart() override { throw std::runtime_error("startup failed"); }
    void execute() override {}
};

class FailingExecuteTask : public PeriodicTask {
public:
    FailingExecuteTask() : PeriodicTask(10ms) {}

protected:
    void execute() override { throw std::runtime_error("execution failed"); }
};

} // namespace

TEST_CASE("periodic task rejects non-positive periods") {
    REQUIRE_THROWS_AS(CountingTask(0ms), std::invalid_argument);
    REQUIRE_THROWS_AS(CountingTask(-1ms), std::invalid_argument);
}

TEST_CASE("periodic task start waits for worker readiness") {
    StartAwareTask task;

    REQUIRE(task.start());
    REQUIRE(task.ready.load());
    REQUIRE(task.state() == PeriodicTask::State::Running);
    task.stop();
    REQUIRE(task.state() == PeriodicTask::State::Stopped);
}

TEST_CASE("periodic task rejects a second active start") {
    CountingTask task(10ms);

    REQUIRE(task.start());
    REQUIRE_THROWS_AS(task.start(), std::logic_error);
    task.stop();
    REQUIRE_NOTHROW(task.stop());
}

TEST_CASE("periodic task reports startup failure without escaping its thread") {
    FailingStartTask task;

    REQUIRE_FALSE(task.start());
    REQUIRE(task.state() == PeriodicTask::State::Failed);
    REQUIRE(task.failure() != nullptr);
    REQUIRE(task.failureMessage() == "startup failed");
}

TEST_CASE("periodic task captures execution failure and invokes its handler") {
    FailingExecuteTask task;
    std::promise<std::string> reportedFailure;
    auto reported = reportedFailure.get_future();
    task.setFailureHandler([&reportedFailure](const std::string& message) {
        reportedFailure.set_value(message);
    });

    REQUIRE(task.start());
    REQUIRE(reported.wait_for(500ms) == std::future_status::ready);
    REQUIRE(reported.get() == "execution failed");
    task.stop();
    REQUIRE(task.state() == PeriodicTask::State::Failed);
    REQUIRE(task.failureMessage() == "execution failed");
}

TEST_CASE("periodic task stop interrupts a long period and destruction joins") {
    const auto startTime = std::chrono::steady_clock::now();
    {
        CountingTask task(10s);
        REQUIRE(task.start());
        while (task.executions.load() == 0) {
            std::this_thread::yield();
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - startTime;
    REQUIRE(elapsed < 500ms);
}
