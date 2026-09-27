#include "codeforces_watcher.h"
#include "mcp_manager.h"
#include "json_value.h"
#include <exception>
#include <iostream>
#include <utility>

CodeforcesContestWatcher::CodeforcesContestWatcher(McpManager& manager, Publish publish)
    : CodeforcesContestWatcher([&manager](std::string& result, std::string& error) {
          return manager.syncCodeforces(result, error);
      }, std::move(publish)) {}
CodeforcesContestWatcher::CodeforcesContestWatcher(Sync sync, Publish publish,
        std::chrono::milliseconds interval)
    : sync_(std::move(sync)), publish_(std::move(publish)), interval_(interval) {}
CodeforcesContestWatcher::~CodeforcesContestWatcher() { stop(); }
void CodeforcesContestWatcher::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_.joinable()) return;
    stopped_ = false;
    worker_ = std::thread([this] { run(); });
}
void CodeforcesContestWatcher::stop() {
    { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}
std::string CodeforcesContestWatcher::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_); return error_;
}
void CodeforcesContestWatcher::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopped_) {
        lock.unlock();
        std::string result, error;
        try {
            if (sync_(result, error)) {
                app_json::JsonValue value;
                if (app_json::JsonParser(result).parse(value, error)) {
                    const auto* success = value.member("success");
                    const auto* count = value.member("new_count");
                    const auto* contests = value.member("new_contests");
                    const auto* checked = value.member("checked_at");
                    if (!success || success->type != app_json::JsonValue::Type::Boolean || !success->boolean ||
                        !count || count->type != app_json::JsonValue::Type::Number ||
                        !contests || contests->type != app_json::JsonValue::Type::Array ||
                        count->text != std::to_string(contests->array.size())) error = "Invalid Codeforces sync result";
                    else {
                        std::cout << "[Codeforces watcher] Sync complete; new contests: " << contests->array.size() << '\n';
                        if (!contests->array.empty()) {
                            publish_("{\"type\":\"codeforces_contests\",\"payload\":{\"new_count\":" + count->text +
                                ",\"contests\":" + app_json::serializeJson(*contests) +
                                (checked ? ",\"checked_at\":" + app_json::serializeJson(*checked) : "") + "}}");
                        }
                    }
                }
            } else if (error.empty()) error = "Codeforces sync failed";
        } catch (const std::exception&) { error = "Codeforces watcher failed"; }
        if (!error.empty()) std::cerr << "[Codeforces watcher] Sync failed; next attempt in one hour\n";
        lock.lock();
        error_ = std::move(error);
        // Wait after completion, including failures: never poll more often than hourly.
        wake_.wait_for(lock, interval_, [this] { return stopped_; });
    }
}
