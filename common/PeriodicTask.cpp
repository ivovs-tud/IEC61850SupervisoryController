#include "PeriodicTask.hpp"

#if defined(PLATFORM_WINDOWS)
#include <windows.h>
#include <synchapi.h>

void PeriodicTask::start() {
    running_.store(true);
    thread_ = std::thread([this]() {
        HANDLE timer = CreateWaitableTimer(NULL, FALSE, NULL);

        LARGE_INTEGER dueTime;
        dueTime.QuadPart = -period_.count() * 10000LL;
        //-5000000LL;  // 500ms in 100ns units (negative = relative)
        SetWaitableTimer(timer, &dueTime, period_.count(), NULL, NULL, FALSE);
        onStart();
        SetThreadPriorityHelper();
        auto nextWakeup = std::chrono::steady_clock::now();
        while (running_.load()) {
            WaitForSingleObject(timer, INFINITE);
            execute();
            /*nextWakeup += period_;
            std::this_thread::sleep_until(nextWakeup);*/
        }
        onStop();
        });
}

void PeriodicTask::SetThreadPriorityHelper() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

#else
void PeriodicTask::SetThreadPriorityHelper() {
    // No-op on non-Windows platforms
}
void PeriodicTask::start() {
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
#endif
