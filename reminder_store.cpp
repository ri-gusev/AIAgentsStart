#include "reminder_store.h"

#include <sqlite3.h>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace {
using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;

Database openDatabase(const std::string& path) {
    sqlite3* raw = nullptr;
    const int code = sqlite3_open_v2(path.c_str(), &raw,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    Database db(raw, sqlite3_close);
    if (code != SQLITE_OK) throw std::runtime_error("Could not open reminders database");
    sqlite3_busy_timeout(db.get(), 5000);
    return db;
}

void execute(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(statement_); }
    void bind(int position, std::int64_t value) {
        if (sqlite3_bind_int64(statement_, position, value) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_));
    }
    bool next() {
        const int code = sqlite3_step(statement_);
        if (code == SQLITE_ROW) return true;
        if (code == SQLITE_DONE) return false;
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    std::int64_t number(int column) const { return sqlite3_column_int64(statement_, column); }
    std::string text(int column) const {
        const auto value = sqlite3_column_text(statement_, column);
        return value ? reinterpret_cast<const char*>(value) : "";
    }
private:
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;
};
}

ReminderStore::ReminderStore(std::string databasePath, const std::string& schemaPath) {
    try {
        if (databasePath.empty()) {
            const char* configured = std::getenv("REMINDERS_DB");
            databasePath = configured && *configured ? configured : "reminders.db";
        }
        databasePath_ = std::filesystem::absolute(databasePath).string();
        std::ifstream file(schemaPath, std::ios::binary);
        if (!file) throw std::runtime_error("Could not read reminders_schema.sql; launch from project root");
        const std::string schema(std::istreambuf_iterator<char>(file), {});
        auto db = openDatabase(databasePath_);
        execute(db.get(), schema.c_str());
    } catch (const std::exception& error) {
        initializationError_ = error.what();
    }
}

bool ReminderStore::isReady() const { return initializationError_.empty(); }
const std::string& ReminderStore::initializationError() const { return initializationError_; }

bool ReminderStore::snapshot(std::vector<Reminder>& reminders,
                            std::vector<ReminderNotification>& notifications, std::string& error) const {
    reminders.clear(); notifications.clear(); error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    try {
        auto db = openDatabase(databasePath_);
        execute(db.get(), "BEGIN");
        Statement rows(db.get(), "SELECT id,text,run_at,status,created_at,triggered_at FROM reminders WHERE status='pending' ORDER BY run_at,id");
        while (rows.next()) reminders.push_back({rows.number(0), rows.text(1), rows.number(2),
            rows.text(3), rows.number(4), rows.number(5)});
        execute(db.get(), "COMMIT");
        return true;
    } catch (const std::exception& problem) { error = problem.what(); return false; }
}

bool ReminderStore::triggerDue(std::int64_t now, std::vector<ReminderNotification>& triggered,
                              std::string& error) const {
    triggered.clear(); error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    try {
        auto db = openDatabase(databasePath_);
        // Claim due reminders atomically by deleting them. The event exists only
        // in memory; concurrent schedulers/deletion cannot claim the same row twice.
        execute(db.get(), "BEGIN IMMEDIATE");
        std::vector<Reminder> due;
        {
            Statement rows(db.get(), "SELECT id,text,run_at FROM reminders WHERE status='pending' AND run_at<=? ORDER BY run_at,id");
            rows.bind(1, now);
            while (rows.next()) due.push_back({rows.number(0), rows.text(1), rows.number(2), "pending", 0, 0});
        }
        for (const auto& reminder : due) {
            Statement update(db.get(), "DELETE FROM reminders WHERE id=? AND status='pending'");
            update.bind(1, reminder.id); update.next();
            if (sqlite3_changes(db.get()) != 1) continue;
            triggered.push_back({reminder.id, reminder.id, reminder.text, now});
        }
        execute(db.get(), "COMMIT");
        return true;
    } catch (const std::exception& problem) {
        // Closing an uncommitted connection rolls its transaction back.
        triggered.clear(); error = problem.what(); return false;
    }
}

bool ReminderStore::deletePending(std::int64_t id, std::string& error) const {
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    if (id <= 0) { error = "Reminder ID must be positive"; return false; }
    try {
        auto db = openDatabase(databasePath_);
        Statement remove(db.get(), "DELETE FROM reminders WHERE id=? AND status='pending'");
        remove.bind(1, id); remove.next();
        if (sqlite3_changes(db.get()) != 1) {
            error = "Reminder not found or already triggered"; return false;
        }
        return true;
    } catch (const std::exception& problem) { error = problem.what(); return false; }
}

std::string reminderUtcTime(std::int64_t timestamp) {
    const std::time_t seconds = static_cast<std::time_t>(timestamp);
    std::tm time{};
#ifdef _WIN32
    gmtime_s(&time, &seconds);
#else
    gmtime_r(&seconds, &time);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &time);
    return buffer;
}
