#include "reminder_scheduler.h"
#include <chrono>
#include <ctime>

ReminderScheduler::ReminderScheduler(const ReminderStore& store, std::function<void()> onTriggered)
    : store_(store), onTriggered_(std::move(onTriggered)) {}
ReminderScheduler::~ReminderScheduler() { stop(); }
void ReminderScheduler::start() {
    if (worker_.joinable()) return;
    { std::lock_guard<std::mutex> lock(mutex_); stopped_ = false; }
    worker_ = std::thread([this] { run(); });
}
void ReminderScheduler::stop() {
    { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}
std::string ReminderScheduler::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_); return error_;
}
void ReminderScheduler::run() {
    for (;;) {
        { std::lock_guard<std::mutex> lock(mutex_); if (stopped_) return; }
        std::vector<ReminderNotification> triggered;
        std::string error;
        store_.triggerDue(static_cast<std::int64_t>(std::time(nullptr)), triggered, error);
        { std::lock_guard<std::mutex> lock(mutex_); error_ = error; }
        if (!triggered.empty()) onTriggered_();
        std::unique_lock<std::mutex> lock(mutex_);
        if (wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stopped_; })) return;
    }
}
