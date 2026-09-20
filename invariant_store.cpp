#include "invariant_store.h"

#include <memory>
#include <sqlite3.h>

namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

bool prepare(sqlite3* database, const char* sql, Statement& statement, std::string& error) {
    sqlite3_stmt* raw = nullptr;
    const int result = sqlite3_prepare_v2(database, sql, -1, &raw, nullptr);
    statement.reset(raw);
    if (result != SQLITE_OK) { error = sqlite3_errmsg(database); return false; }
    return true;
}

bool bindText(sqlite3* database, sqlite3_stmt* statement, int index,
              const std::string& text, std::string& error) {
    if (sqlite3_bind_text(statement, index, text.data(), static_cast<int>(text.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        error = sqlite3_errmsg(database); return false;
    }
    return true;
}

bool stepDone(sqlite3* database, sqlite3_stmt* statement, std::string& error) {
    if (sqlite3_step(statement) != SQLITE_DONE) { error = sqlite3_errmsg(database); return false; }
    return true;
}

std::string columnText(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? std::string(reinterpret_cast<const char*>(value),
                               static_cast<std::size_t>(sqlite3_column_bytes(statement, column)))
                 : std::string{};
}
}

InvariantStore::InvariantStore(const std::string& databasePath) {
    if (sqlite3_open(databasePath.c_str(), &database_) != SQLITE_OK) {
        initializationError_ = database_ ? sqlite3_errmsg(database_) : "Could not open invariants database";
        return;
    }
    if (sqlite3_busy_timeout(database_, 3000) != SQLITE_OK ||
        sqlite3_exec(database_,
            "CREATE TABLE IF NOT EXISTS project_invariants ("
            "project_id TEXT NOT NULL, invariant_key TEXT NOT NULL, invariant_value TEXT NOT NULL,"
            "description TEXT NOT NULL, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "PRIMARY KEY(project_id, invariant_key));",
            nullptr, nullptr, nullptr) != SQLITE_OK) {
        initializationError_ = sqlite3_errmsg(database_);
    }
}

InvariantStore::~InvariantStore() { if (database_) sqlite3_close(database_); }
bool InvariantStore::isReady() const { return database_ && initializationError_.empty(); }
const std::string& InvariantStore::initializationError() const { return initializationError_; }

bool InvariantStore::load(const std::string& projectId, std::vector<ProjectInvariant>& invariants,
                          std::string& error) const {
    invariants.clear(); error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    Statement statement(nullptr, sqlite3_finalize);
    if (!prepare(database_, "SELECT invariant_key, invariant_value, description FROM project_invariants "
                            "WHERE project_id=?1 ORDER BY invariant_key;", statement, error) ||
        !bindText(database_, statement.get(), 1, projectId, error)) return false;
    int result = SQLITE_OK;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
        invariants.push_back({columnText(statement.get(), 0), columnText(statement.get(), 1),
                              columnText(statement.get(), 2)});
    }
    if (result != SQLITE_DONE) { invariants.clear(); error = sqlite3_errmsg(database_); return false; }
    return true;
}

bool InvariantStore::create(const std::string& projectId, const ProjectInvariant& invariant,
                            std::string& error) {
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    Statement statement(nullptr, sqlite3_finalize);
    return prepare(database_, "INSERT INTO project_invariants(project_id, invariant_key, invariant_value, description, updated_at) "
                              "VALUES(?1, ?2, ?3, ?4, CURRENT_TIMESTAMP);", statement, error) &&
           bindText(database_, statement.get(), 1, projectId, error) &&
           bindText(database_, statement.get(), 2, invariant.key, error) &&
           bindText(database_, statement.get(), 3, invariant.value, error) &&
           bindText(database_, statement.get(), 4, invariant.description, error) &&
           stepDone(database_, statement.get(), error);
}

bool InvariantStore::update(const std::string& projectId, const std::string& currentKey,
                            const ProjectInvariant& invariant, std::string& error) {
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    Statement statement(nullptr, sqlite3_finalize);
    if (!prepare(database_, "UPDATE project_invariants SET invariant_key=?1, invariant_value=?2, description=?3, "
                            "updated_at=CURRENT_TIMESTAMP WHERE project_id=?4 AND invariant_key=?5;", statement, error) ||
        !bindText(database_, statement.get(), 1, invariant.key, error) ||
        !bindText(database_, statement.get(), 2, invariant.value, error) ||
        !bindText(database_, statement.get(), 3, invariant.description, error) ||
        !bindText(database_, statement.get(), 4, projectId, error) ||
        !bindText(database_, statement.get(), 5, currentKey, error) || !stepDone(database_, statement.get(), error)) return false;
    if (sqlite3_changes(database_) != 1) { error = "Invariant not found"; return false; }
    return true;
}

bool InvariantStore::remove(const std::string& projectId, const std::string& key, std::string& error) {
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    Statement statement(nullptr, sqlite3_finalize);
    if (!prepare(database_, "DELETE FROM project_invariants WHERE project_id=?1 AND invariant_key=?2;", statement, error) ||
        !bindText(database_, statement.get(), 1, projectId, error) ||
        !bindText(database_, statement.get(), 2, key, error) || !stepDone(database_, statement.get(), error)) return false;
    if (sqlite3_changes(database_) != 1) { error = "Invariant not found"; return false; }
    return true;
}

bool InvariantStore::removeProject(const std::string& projectId, std::string& error) {
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    Statement statement(nullptr, sqlite3_finalize);
    return prepare(database_, "DELETE FROM project_invariants WHERE project_id=?1;", statement, error) &&
           bindText(database_, statement.get(), 1, projectId, error) &&
           stepDone(database_, statement.get(), error);
}
