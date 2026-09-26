#pragma once

#include "reminder_store.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

class ReminderScheduler {
public:
    ReminderScheduler(const ReminderStore& store, std::function<void()> onTriggered);
    ~ReminderScheduler();
    void start();
    void stop();
    std::string lastError() const;
private:
    void run();
    const ReminderStore& store_;
    std::function<void()> onTriggered_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopped_ = false;
    std::string error_;
    std::thread worker_;
};
