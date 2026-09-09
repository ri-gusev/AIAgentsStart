#include "memory_store.h"

#include <sqlite3.h>

namespace {
constexpr const char* kCreateTableSql =
    "CREATE TABLE IF NOT EXISTS long_term_memory ("
    "memory_key TEXT PRIMARY KEY NOT NULL,"
    "memory_value TEXT NOT NULL,"
    "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
    ");";
}

MemoryStore::MemoryStore(const std::string& databasePath) {
    if (sqlite3_open(databasePath.c_str(), &database_) != SQLITE_OK) {
        initializationError_ = database_ ? sqlite3_errmsg(database_) : "Could not open SQLite database";
        return;
    }
    sqlite3_busy_timeout(database_, 3000);
    char* sqliteError = nullptr;
    if (sqlite3_exec(database_, kCreateTableSql, nullptr, nullptr, &sqliteError) != SQLITE_OK) {
        initializationError_ = sqliteError ? sqliteError : "Could not create long-term memory table";
        sqlite3_free(sqliteError);
    }
}

MemoryStore::~MemoryStore() {
    if (database_) sqlite3_close(database_);
}

bool MemoryStore::isReady() const {
    return database_ && initializationError_.empty();
}

const std::string& MemoryStore::initializationError() const {
    return initializationError_;
}

bool MemoryStore::loadAll(std::vector<LongTermMemoryFact>& facts, std::string& error) const {
    facts.clear();
    if (!isReady()) { error = initializationError_; return false; }

    sqlite3_stmt* statement = nullptr;
    const char* sql = "SELECT memory_key, memory_value FROM long_term_memory ORDER BY memory_key;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* key = sqlite3_column_text(statement, 0);
        const auto* value = sqlite3_column_text(statement, 1);
        if (key && value) {
            facts.push_back({reinterpret_cast<const char*>(key),
                             reinterpret_cast<const char*>(value)});
        }
    }
    const int result = sqlite3_errcode(database_);
    sqlite3_finalize(statement);
    if (result != SQLITE_OK && result != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    return true;
}

bool MemoryStore::upsert(const LongTermMemoryFact& fact, std::string& error) {
    if (!isReady()) { error = initializationError_; return false; }
    const char* sql =
        "INSERT INTO long_term_memory(memory_key, memory_value, updated_at) "
        "VALUES(?1, ?2, CURRENT_TIMESTAMP) "
        "ON CONFLICT(memory_key) DO UPDATE SET "
        "memory_value=excluded.memory_value, updated_at=CURRENT_TIMESTAMP;";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    sqlite3_bind_text(statement, 1, fact.key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, fact.value.c_str(), -1, SQLITE_TRANSIENT);
    const int result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    return true;
}
