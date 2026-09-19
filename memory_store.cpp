#include "memory_store.h"

#include <charconv>
#include <limits>
#include <memory>
#include <sqlite3.h>

namespace {
constexpr const char* kCreateTableSql =
    "PRAGMA foreign_keys=ON;"
    "CREATE TABLE IF NOT EXISTS long_term_memory ("
    "memory_key TEXT PRIMARY KEY NOT NULL,"
    "memory_value TEXT NOT NULL,"
    "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
    ");"
    "CREATE TABLE IF NOT EXISTS chats ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "name TEXT NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS working_memory ("
    "chat_id INTEGER NOT NULL REFERENCES chats(id),"
    "memory_key TEXT NOT NULL,"
    "memory_value TEXT NOT NULL,"
    "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "PRIMARY KEY(chat_id, memory_key)"
    ");"
    "CREATE TABLE IF NOT EXISTS project_summaries ("
    "chat_id INTEGER PRIMARY KEY REFERENCES chats(id),"
    "summary TEXT NOT NULL DEFAULT '',"
    "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
    ");"
    "CREATE TABLE IF NOT EXISTS project_task_states ("
    "chat_id INTEGER PRIMARY KEY REFERENCES chats(id),"
    "state TEXT NOT NULL DEFAULT 'planning' CHECK(state IN ('planning','execution','validation','DONE')),"
    "plan TEXT NOT NULL DEFAULT '',"
    "validation_report TEXT NOT NULL DEFAULT '',"
    "paused INTEGER NOT NULL DEFAULT 0 CHECK(paused IN (0,1)),"
    "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
    ");"
    "CREATE TABLE IF NOT EXISTS agent_settings ("
    "setting_key TEXT PRIMARY KEY NOT NULL,"
    "setting_value TEXT NOT NULL"
    ");";

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

bool prepare(sqlite3* database, const char* sql, Statement& statement, std::string& error) {
    sqlite3_stmt* rawStatement = nullptr;
    const int result = sqlite3_prepare_v2(database, sql, -1, &rawStatement, nullptr);
    statement.reset(rawStatement);
    if (result != SQLITE_OK) {
        error = sqlite3_errmsg(database);
        return false;
    }
    return true;
}

bool bindText(sqlite3* database, sqlite3_stmt* statement, int index,
              const std::string& text, std::string& error) {
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "SQLite text value is too large";
        return false;
    }
    if (sqlite3_bind_text(statement, index, text.data(), static_cast<int>(text.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        error = sqlite3_errmsg(database);
        return false;
    }
    return true;
}

bool bindChatId(sqlite3* database, sqlite3_stmt* statement, int index,
                const std::string& text, std::string& error) {
    sqlite3_int64 chatId = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), chatId);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || chatId <= 0) {
        error = "Invalid chat identifier";
        return false;
    }
    if (sqlite3_bind_int64(statement, index, chatId) != SQLITE_OK) {
        error = sqlite3_errmsg(database);
        return false;
    }
    return true;
}

bool bindInt(sqlite3* database, sqlite3_stmt* statement, int index,
             int value, std::string& error) {
    if (sqlite3_bind_int(statement, index, value) != SQLITE_OK) {
        error = sqlite3_errmsg(database);
        return false;
    }
    return true;
}

std::string columnText(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    if (!value) return {};
    return std::string(reinterpret_cast<const char*>(value),
                       static_cast<std::size_t>(sqlite3_column_bytes(statement, column)));
}

bool execute(sqlite3* database, const char* sql, std::string& error) {
    char* sqliteError = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &sqliteError);
    if (result != SQLITE_OK) {
        error = sqliteError ? sqliteError : sqlite3_errmsg(database);
        sqlite3_free(sqliteError);
        return false;
    }
    return true;
}

bool stepDone(sqlite3* database, sqlite3_stmt* statement, std::string& error) {
    if (sqlite3_step(statement) != SQLITE_DONE) {
        error = sqlite3_errmsg(database);
        return false;
    }
    return true;
}
}

MemoryStore::MemoryStore(const std::string& databasePath) {
    if (sqlite3_open(databasePath.c_str(), &database_) != SQLITE_OK) {
        initializationError_ = database_ ? sqlite3_errmsg(database_) : "Could not open SQLite database";
        return;
    }
    if (sqlite3_busy_timeout(database_, 3000) != SQLITE_OK) {
        initializationError_ = sqlite3_errmsg(database_);
        return;
    }
    execute(database_, kCreateTableSql, initializationError_);
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
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    Statement statement(nullptr, sqlite3_finalize);
    const char* sql = "SELECT memory_key, memory_value FROM long_term_memory ORDER BY memory_key;";
    if (!prepare(database_, sql, statement, error)) return false;
    int result;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
        facts.push_back({columnText(statement.get(), 0), columnText(statement.get(), 1)});
    }
    if (result != SQLITE_DONE) {
        facts.clear();
        error = sqlite3_errmsg(database_);
        return false;
    }
    return true;
}

bool MemoryStore::upsert(const LongTermMemoryFact& fact, std::string& error) {
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    const char* sql =
        "INSERT INTO long_term_memory(memory_key, memory_value, updated_at) "
        "VALUES(?1, ?2, CURRENT_TIMESTAMP) "
        "ON CONFLICT(memory_key) DO UPDATE SET "
        "memory_value=excluded.memory_value, updated_at=CURRENT_TIMESTAMP;";
    Statement statement(nullptr, sqlite3_finalize);
    return prepare(database_, sql, statement, error) &&
           bindText(database_, statement.get(), 1, fact.key, error) &&
           bindText(database_, statement.get(), 2, fact.value, error) &&
           stepDone(database_, statement.get(), error);
}

bool MemoryStore::loadChats(std::vector<StoredChat>& chats, std::string& error) const {
    chats.clear();
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    Statement statement(nullptr, sqlite3_finalize);
    if (!prepare(database_, "SELECT id, name FROM chats ORDER BY id;", statement, error)) return false;
    int result;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
        chats.push_back({std::to_string(sqlite3_column_int64(statement.get(), 0)),
                         columnText(statement.get(), 1)});
    }
    if (result != SQLITE_DONE) {
        chats.clear();
        error = sqlite3_errmsg(database_);
        return false;
    }
    return true;
}

bool MemoryStore::createChat(const std::string& name, std::string& id, std::string& error) {
    id.clear();
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    if (name.empty()) { error = "Chat name must not be empty"; return false; }
    if (!execute(database_, "BEGIN IMMEDIATE;", error)) return false;
    bool success = true;
    {
        Statement statement(nullptr, sqlite3_finalize);
        success = prepare(database_, "INSERT INTO chats(name) VALUES(?1);", statement, error) &&
                  bindText(database_, statement.get(), 1, name, error) &&
                  stepDone(database_, statement.get(), error);
        if (success) id = std::to_string(sqlite3_last_insert_rowid(database_));
    }
    if (success) {
        Statement statement(nullptr, sqlite3_finalize);
        success = prepare(database_, "INSERT INTO project_summaries(chat_id) VALUES(?1);",
                          statement, error) &&
                  bindChatId(database_, statement.get(), 1, id, error) &&
                  stepDone(database_, statement.get(), error);
    }
    if (success) {
        Statement statement(nullptr, sqlite3_finalize);
        success = prepare(database_, "INSERT INTO project_task_states(chat_id) VALUES(?1);",
                          statement, error) &&
                  bindChatId(database_, statement.get(), 1, id, error) &&
                  stepDone(database_, statement.get(), error);
    }
    if (success && execute(database_, "COMMIT;", error)) return true;
    std::string ignoredRollbackError;
    execute(database_, "ROLLBACK;", ignoredRollbackError);
    id.clear();
    return false;
}

bool MemoryStore::ensureProjectData(const std::string& chatId, std::string& error) {
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    if (!execute(database_, "BEGIN IMMEDIATE;", error)) return false;
    bool success = true;
    {
        Statement statement(nullptr, sqlite3_finalize);
        success = prepare(database_, "INSERT OR IGNORE INTO project_summaries(chat_id) VALUES(?1);",
                          statement, error) &&
                  bindChatId(database_, statement.get(), 1, chatId, error) &&
                  stepDone(database_, statement.get(), error);
    }
    if (success) {
        Statement statement(nullptr, sqlite3_finalize);
        success = prepare(database_, "INSERT OR IGNORE INTO project_task_states(chat_id) VALUES(?1);",
                          statement, error) &&
                  bindChatId(database_, statement.get(), 1, chatId, error) &&
                  stepDone(database_, statement.get(), error);
    }
    if (success && execute(database_, "COMMIT;", error)) return true;
    std::string ignoredRollbackError;
    execute(database_, "ROLLBACK;", ignoredRollbackError);
    return false;
}

bool MemoryStore::loadProjectSummary(const std::string& chatId, std::string& summary,
                                     std::string& error) const {
    summary.clear();
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    Statement statement(nullptr, sqlite3_finalize);
    if (!prepare(database_, "SELECT summary FROM project_summaries WHERE chat_id=?1;",
                 statement, error) || !bindChatId(database_, statement.get(), 1, chatId, error)) {
        return false;
    }
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return true;
    if (result == SQLITE_ROW) {
        summary = columnText(statement.get(), 0);
        if (sqlite3_step(statement.get()) == SQLITE_DONE) return true;
        summary.clear();
        error = sqlite3_errmsg(database_);
        return false;
    }
    error = sqlite3_errmsg(database_);
    return false;
}

bool MemoryStore::saveProjectPause(const std::string& chatId, const std::string& summary,
                                   const ProjectTaskState& taskState, std::string& error) {
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    if (taskState.state != "planning" && taskState.state != "execution" &&
        taskState.state != "validation" && taskState.state != "DONE") {
        error = "Invalid task state";
        return false;
    }
    if (!execute(database_, "BEGIN IMMEDIATE;", error)) return false;
    bool success = true;
    {
        Statement statement(nullptr, sqlite3_finalize);
        const char* sql =
            "INSERT INTO project_summaries(chat_id, summary, updated_at) VALUES(?1, ?2, CURRENT_TIMESTAMP) "
            "ON CONFLICT(chat_id) DO UPDATE SET summary=excluded.summary, updated_at=CURRENT_TIMESTAMP;";
        success = prepare(database_, sql, statement, error) &&
                  bindChatId(database_, statement.get(), 1, chatId, error) &&
                  bindText(database_, statement.get(), 2, summary, error) &&
                  stepDone(database_, statement.get(), error);
    }
    if (success) {
        Statement statement(nullptr, sqlite3_finalize);
        const char* sql =
            "INSERT INTO project_task_states(chat_id, state, plan, validation_report, paused, updated_at) "
            "VALUES(?1, ?2, ?3, ?4, ?5, CURRENT_TIMESTAMP) "
            "ON CONFLICT(chat_id) DO UPDATE SET state=excluded.state, plan=excluded.plan, "
            "validation_report=excluded.validation_report, paused=excluded.paused, "
            "updated_at=CURRENT_TIMESTAMP;";
        success = prepare(database_, sql, statement, error) &&
                  bindChatId(database_, statement.get(), 1, chatId, error) &&
                  bindText(database_, statement.get(), 2, taskState.state, error) &&
                  bindText(database_, statement.get(), 3, taskState.plan, error) &&
                  bindText(database_, statement.get(), 4, taskState.validationReport, error) &&
                  bindInt(database_, statement.get(), 5, taskState.paused ? 1 : 0, error) &&
                  stepDone(database_, statement.get(), error);
    }
    if (success && execute(database_, "COMMIT;", error)) return true;
    std::string ignoredRollbackError;
    execute(database_, "ROLLBACK;", ignoredRollbackError);
    return false;
}

bool MemoryStore::loadTaskState(const std::string& chatId, ProjectTaskState& taskState,
                                std::string& error) const {
    taskState = ProjectTaskState{};
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    Statement statement(nullptr, sqlite3_finalize);
    if (!prepare(database_, "SELECT state, plan, validation_report, paused "
                            "FROM project_task_states WHERE chat_id=?1;", statement, error) ||
        !bindChatId(database_, statement.get(), 1, chatId, error)) return false;
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return true;
    if (result == SQLITE_ROW) {
        taskState.state = columnText(statement.get(), 0);
        taskState.plan = columnText(statement.get(), 1);
        taskState.validationReport = columnText(statement.get(), 2);
        taskState.paused = sqlite3_column_int(statement.get(), 3) != 0;
        if (sqlite3_step(statement.get()) == SQLITE_DONE) return true;
        taskState = ProjectTaskState{};
        error = sqlite3_errmsg(database_);
        return false;
    }
    error = sqlite3_errmsg(database_);
    return false;
}

bool MemoryStore::saveTaskState(const std::string& chatId, const ProjectTaskState& taskState,
                                std::string& error) {
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    if (taskState.state != "planning" && taskState.state != "execution" &&
        taskState.state != "validation" && taskState.state != "DONE") {
        error = "Invalid task state";
        return false;
    }
    Statement statement(nullptr, sqlite3_finalize);
    const char* sql =
        "INSERT INTO project_task_states(chat_id, state, plan, validation_report, paused, updated_at) "
        "VALUES(?1, ?2, ?3, ?4, ?5, CURRENT_TIMESTAMP) "
        "ON CONFLICT(chat_id) DO UPDATE SET state=excluded.state, plan=excluded.plan, "
        "validation_report=excluded.validation_report, paused=excluded.paused, "
        "updated_at=CURRENT_TIMESTAMP;";
    return prepare(database_, sql, statement, error) &&
           bindChatId(database_, statement.get(), 1, chatId, error) &&
           bindText(database_, statement.get(), 2, taskState.state, error) &&
           bindText(database_, statement.get(), 3, taskState.plan, error) &&
           bindText(database_, statement.get(), 4, taskState.validationReport, error) &&
           bindInt(database_, statement.get(), 5, taskState.paused ? 1 : 0, error) &&
           stepDone(database_, statement.get(), error);
}

bool MemoryStore::deleteChat(const std::string& chatId, std::string& replacementId,
                             std::string& error) {
    replacementId.clear();
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    if (!execute(database_, "BEGIN IMMEDIATE;", error)) return false;

    bool success = true;
    {
        Statement statement(nullptr, sqlite3_finalize);
        if (!prepare(database_, "SELECT 1 FROM chats WHERE id=?1;", statement, error) ||
            !bindChatId(database_, statement.get(), 1, chatId, error)) {
            success = false;
        } else {
            const int result = sqlite3_step(statement.get());
            if (result == SQLITE_DONE) {
                error = "Chat not found";
                success = false;
            } else if (result != SQLITE_ROW) {
                error = sqlite3_errmsg(database_);
                success = false;
            }
        }
    }

    if (success) {
        Statement statement(nullptr, sqlite3_finalize);
        success = prepare(database_, "DELETE FROM project_summaries WHERE chat_id=?1;",
                          statement, error) &&
                  bindChatId(database_, statement.get(), 1, chatId, error) &&
                  stepDone(database_, statement.get(), error);
    }

    if (success) {
        Statement statement(nullptr, sqlite3_finalize);
        success = prepare(database_, "DELETE FROM project_task_states WHERE chat_id=?1;",
                          statement, error) &&
                  bindChatId(database_, statement.get(), 1, chatId, error) &&
                  stepDone(database_, statement.get(), error);
    }

    if (success) {
        Statement statement(nullptr, sqlite3_finalize);
        success = prepare(database_, "DELETE FROM working_memory WHERE chat_id=?1;",
                          statement, error) &&
                  bindChatId(database_, statement.get(), 1, chatId, error) &&
                  stepDone(database_, statement.get(), error);
    }

    if (success) {
        Statement statement(nullptr, sqlite3_finalize);
        success = prepare(database_, "DELETE FROM chats WHERE id=?1;", statement, error) &&
                  bindChatId(database_, statement.get(), 1, chatId, error) &&
                  stepDone(database_, statement.get(), error);
        if (success && sqlite3_changes(database_) != 1) {
            error = "Chat not found";
            success = false;
        }
    }

    bool hasRemainingChat = false;
    if (success) {
        Statement statement(nullptr, sqlite3_finalize);
        if (!prepare(database_, "SELECT 1 FROM chats LIMIT 1;", statement, error)) {
            success = false;
        } else {
            const int result = sqlite3_step(statement.get());
            if (result == SQLITE_ROW) hasRemainingChat = true;
            else if (result != SQLITE_DONE) {
                error = sqlite3_errmsg(database_);
                success = false;
            }
        }
    }

    if (success && !hasRemainingChat) {
        Statement statement(nullptr, sqlite3_finalize);
        const std::string defaultName = "Основной";
        success = prepare(database_, "INSERT INTO chats(name) VALUES(?1);", statement, error) &&
                  bindText(database_, statement.get(), 1, defaultName, error) &&
                  stepDone(database_, statement.get(), error);
        if (success) replacementId = std::to_string(sqlite3_last_insert_rowid(database_));
    }

    if (success && !replacementId.empty()) {
        Statement statement(nullptr, sqlite3_finalize);
        success = prepare(database_, "INSERT INTO project_summaries(chat_id) VALUES(?1);",
                          statement, error) &&
                  bindChatId(database_, statement.get(), 1, replacementId, error) &&
                  stepDone(database_, statement.get(), error);
    }

    if (success && !replacementId.empty()) {
        Statement statement(nullptr, sqlite3_finalize);
        success = prepare(database_, "INSERT INTO project_task_states(chat_id) VALUES(?1);",
                          statement, error) &&
                  bindChatId(database_, statement.get(), 1, replacementId, error) &&
                  stepDone(database_, statement.get(), error);
    }

    if (success && execute(database_, "COMMIT;", error)) return true;

    std::string ignoredRollbackError;
    execute(database_, "ROLLBACK;", ignoredRollbackError);
    replacementId.clear();
    return false;
}

bool MemoryStore::loadWorking(const std::string& chatId,
                              std::vector<LongTermMemoryFact>& facts, std::string& error) const {
    facts.clear();
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    Statement statement(nullptr, sqlite3_finalize);
    if (!prepare(database_, "SELECT memory_key, memory_value FROM working_memory "
                            "WHERE chat_id=?1 ORDER BY memory_key;", statement, error) ||
        !bindChatId(database_, statement.get(), 1, chatId, error)) return false;
    int result;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
        facts.push_back({columnText(statement.get(), 0), columnText(statement.get(), 1)});
    }
    if (result != SQLITE_DONE) {
        facts.clear();
        error = sqlite3_errmsg(database_);
        return false;
    }
    return true;
}

bool MemoryStore::upsertWorking(const std::string& chatId, const LongTermMemoryFact& fact,
                                std::string& error) {
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    const char* sql =
        "INSERT INTO working_memory(chat_id, memory_key, memory_value, updated_at) "
        "VALUES(?1, ?2, ?3, CURRENT_TIMESTAMP) "
        "ON CONFLICT(chat_id, memory_key) DO UPDATE SET "
        "memory_value=excluded.memory_value, updated_at=CURRENT_TIMESTAMP;";
    Statement statement(nullptr, sqlite3_finalize);
    return prepare(database_, sql, statement, error) &&
           bindChatId(database_, statement.get(), 1, chatId, error) &&
           bindText(database_, statement.get(), 2, fact.key, error) &&
           bindText(database_, statement.get(), 3, fact.value, error) &&
           stepDone(database_, statement.get(), error);
}

bool MemoryStore::loadSetting(const std::string& key, std::string& value, std::string& error) const {
    value.clear();
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    Statement statement(nullptr, sqlite3_finalize);
    if (!prepare(database_, "SELECT setting_value FROM agent_settings WHERE setting_key=?1;",
                 statement, error) || !bindText(database_, statement.get(), 1, key, error)) return false;
    int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return true;
    if (result == SQLITE_ROW) {
        value = columnText(statement.get(), 0);
        result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) return true;
    }
    value.clear();
    error = sqlite3_errmsg(database_);
    return false;
}

bool MemoryStore::saveSetting(const std::string& key, const std::string& value, std::string& error) {
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    Statement statement(nullptr, sqlite3_finalize);
    const char* sql =
        "INSERT INTO agent_settings(setting_key, setting_value) VALUES(?1, ?2) "
        "ON CONFLICT(setting_key) DO UPDATE SET setting_value=excluded.setting_value;";
    return prepare(database_, sql, statement, error) &&
           bindText(database_, statement.get(), 1, key, error) &&
           bindText(database_, statement.get(), 2, value, error) &&
           stepDone(database_, statement.get(), error);
}

bool MemoryStore::saveFacts(const std::string& chatId,
                            const std::vector<LongTermMemoryFact>& working,
                            const std::vector<LongTermMemoryFact>& longTerm, std::string& error) {
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    if (!execute(database_, "BEGIN IMMEDIATE;", error)) return false;
    bool success = true;
    for (const auto& fact : working) {
        if (!upsertWorking(chatId, fact, error)) { success = false; break; }
    }
    if (success) {
        for (const auto& fact : longTerm) {
            if (!upsert(fact, error)) { success = false; break; }
        }
    }
    if (success && execute(database_, "COMMIT;", error)) return true;
    std::string ignoredRollbackError;
    execute(database_, "ROLLBACK;", ignoredRollbackError);
    return false;
}
