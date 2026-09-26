#include "reminder_store.h"
#include "reminder_scheduler.h"
#include <sqlite3.h>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void insert(const std::string& path, std::int64_t at) {
    sqlite3* db = nullptr;
    require(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "open test database");
    sqlite3_stmt* statement = nullptr;
    const int prepared = sqlite3_prepare_v2(db,
        "INSERT INTO reminders(text,run_at,status,created_at) VALUES ('Test reminder',?,'pending',?)", -1, &statement, nullptr);
    if (prepared == SQLITE_OK) {
        sqlite3_bind_int64(statement, 1, at);
        sqlite3_bind_int64(statement, 2, std::time(nullptr));
    }
    const int result = prepared == SQLITE_OK ? sqlite3_step(statement) : prepared;
    sqlite3_finalize(statement); sqlite3_close(db);
    require(result == SQLITE_DONE, "insert test reminder");
}
}

int main() {
    const auto path = std::filesystem::temp_directory_path() /
        ("agent-reminders-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    try {
        const std::string schema = std::string(PROJECT_SOURCE_ROOT) + "/mcp_server/reminders_schema.sql";
        ReminderStore first(path.string(), schema), second(path.string(), schema);
        require(first.isReady() && second.isReady(), "initialize isolated reminder stores");
        const auto now = static_cast<std::int64_t>(std::time(nullptr));
        insert(path.string(), now - 10); insert(path.string(), now + 3600);
        std::vector<Reminder> reminders;
        std::vector<ReminderNotification> notifications;
        std::string error;
        require(second.snapshot(reminders, notifications, error) && reminders.size() == 2 && notifications.empty(),
                "pending reminders survive reopening the database");
        std::vector<ReminderNotification> a, b;
        bool aSuccess = false, bSuccess = false;
        std::thread one([&] { std::string detail; aSuccess = first.triggerDue(now, a, detail); });
        std::thread two([&] { std::string detail; bSuccess = second.triggerDue(now, b, detail); });
        one.join(); two.join();
        require(aSuccess && bSuccess && a.size() + b.size() == 1, "concurrent ticks trigger exactly once");
        require(first.snapshot(reminders, notifications, error) && notifications.size() == 1 &&
                reminders[0].status == "triggered" && reminders[0].triggeredAt == now && reminders[1].status == "pending",
                "status, triggered_at and durable notification commit together");
        require(first.triggerDue(now, a, error) && a.empty(), "repeat tick does not repeat notification");

        insert(path.string(), now - 1);
        std::mutex mutex;
        std::condition_variable wake;
        bool received = false;
        ReminderScheduler scheduler(second, [&] { std::lock_guard<std::mutex> lock(mutex); received = true; wake.notify_one(); });
        scheduler.start();
        {
            std::unique_lock<std::mutex> lock(mutex);
            require(wake.wait_for(lock, std::chrono::seconds(3), [&] { return received; }),
                    "background scheduler catches overdue pending reminders without a request");
        }
        scheduler.stop();
        require(first.snapshot(reminders, notifications, error) && notifications.size() == 2,
                "scheduler persists its notification");
        ReminderStore restarted(path.string(), schema);
        require(restarted.triggerDue(now + 3600, a, error) && a.size() == 1,
                "saved future pending reminder resumes after restart");
        std::cout << "Reminder persistence, atomic triggering, concurrency and background scheduler passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + "-wal");
    std::filesystem::remove(path.string() + "-shm");
    return 0;
}
