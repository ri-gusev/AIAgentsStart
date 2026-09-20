#include "memory_store.h"

#include <charconv>
#include <limits>
#include <memory>
#include <sqlite3.h>

namespace {
constexpr const char* kCreateTaskStateTableSql =
    "CREATE TABLE IF NOT EXISTS project_task_states ("
    "chat_id INTEGER PRIMARY KEY REFERENCES chats(id),"
    "state TEXT NOT NULL DEFAULT 'PLANNING' "
        "CHECK(state IN ('PLANNING','EXECUTION','VALIDATION','DONE','PAUSED')),"
    "resume_state TEXT DEFAULT NULL "
        "CHECK(resume_state IS NULL OR resume_state IN ('PLANNING','EXECUTION','VALIDATION','DONE')),"
    "plan TEXT NOT NULL DEFAULT '',"
    "validation_report TEXT NOT NULL DEFAULT '',"
    "execution_completed INTEGER NOT NULL DEFAULT 0 CHECK(execution_completed IN (0,1)),"
    "validation_passed INTEGER NOT NULL DEFAULT 0 CHECK(validation_passed IN (0,1)),"
    "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "CHECK((state='PAUSED' AND resume_state IS NOT NULL) OR "
          "(state<>'PAUSED' AND resume_state IS NULL))"
    ");";

constexpr const char* kCreateTransitionLogSql =
    "CREATE TABLE IF NOT EXISTS task_state_transition_log ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "chat_id INTEGER NOT NULL REFERENCES chats(id) ON DELETE CASCADE,"
    "previous_state TEXT DEFAULT NULL "
        "CHECK(previous_state IS NULL OR previous_state IN "
              "('PLANNING','EXECUTION','VALIDATION','DONE','PAUSED')),"
    "action TEXT NOT NULL CHECK(action IN "
        "('CREATE_TASK','APPROVE_PLAN','REGENERATE_PLAN','EXECUTION_FINISHED',"
         "'VALIDATION_PASSED','VALIDATION_FAILED','EXECUTION_RESULT_READY',"
         "'VALIDATION_RESULT_READY','PAUSE','RESUME'))," 
    "new_state TEXT NOT NULL "
        "CHECK(new_state IN ('PLANNING','EXECUTION','VALIDATION','DONE','PAUSED')),"
    "timestamp TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_task_state_transition_log_chat_id_id "
    "ON task_state_transition_log(chat_id, id);";

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

bool bindNullableText(sqlite3* database, sqlite3_stmt* statement, int index,
                      const std::string& text, std::string& error) {
    if (text.empty()) {
        if (sqlite3_bind_null(statement, index) != SQLITE_OK) {
            error = sqlite3_errmsg(database);
            return false;
        }
        return true;
    }
    return bindText(database, statement, index, text, error);
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

bool stepDone(sqlite3* database, sqlite3_stmt* statement, std::string& error);

bool isTaskState(const std::string& state) {
    return state == "PLANNING" || state == "EXECUTION" || state == "VALIDATION" ||
           state == "DONE" || state == "PAUSED";
}

bool isResumableTaskState(const std::string& state) {
    return state == "PLANNING" || state == "EXECUTION" || state == "VALIDATION" ||
           state == "DONE";
}

bool isAllowedTaskTransition(const std::string& previousState, const std::string& action,
                             const std::string& newState) {
    if (action == "CREATE_TASK") {
        return (previousState == "PLANNING" || previousState == "DONE") &&
               newState == "PLANNING";
    }
    if (action == "REGENERATE_PLAN") {
        return previousState == "PLANNING" && newState == "PLANNING";
    }
    if (action == "APPROVE_PLAN") {
        return previousState == "PLANNING" && newState == "EXECUTION";
    }
    if (action == "EXECUTION_FINISHED") {
        return previousState == "EXECUTION" && newState == "VALIDATION";
    }
    if (action == "EXECUTION_RESULT_READY") {
        return previousState == "EXECUTION" && newState == "EXECUTION";
    }
    if (action == "VALIDATION_RESULT_READY") {
        return previousState == "VALIDATION" && newState == "VALIDATION";
    }
    if (action == "VALIDATION_PASSED") {
        return previousState == "VALIDATION" && newState == "DONE";
    }
    if (action == "VALIDATION_FAILED") {
        return previousState == "VALIDATION" && newState == "EXECUTION";
    }
    if (action == "PAUSE") {
        return (previousState == "PLANNING" || previousState == "EXECUTION") &&
               newState == "PAUSED";
    }
    if (action == "RESUME") {
        return previousState == "PAUSED" && isResumableTaskState(newState);
    }
    return false;
}

bool hasTableColumn(sqlite3* database, const char* table, const char* column,
                    bool& found, std::string& error) {
    found = false;
    const std::string sql = std::string("PRAGMA table_info(") + table + ");";
    Statement statement(nullptr, sqlite3_finalize);
    if (!prepare(database, sql.c_str(), statement, error)) return false;
    int result = SQLITE_OK;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
        if (columnText(statement.get(), 1) == column) {
            found = true;
            return true;
        }
    }
    if (result != SQLITE_DONE) {
        error = sqlite3_errmsg(database);
        return false;
    }
    return true;
}

bool ensureExecutionCompletedColumn(sqlite3* database, std::string& error) {
    bool found = false;
    if (!hasTableColumn(database, "project_task_states", "execution_completed", found, error)) {
        return false;
    }
    if (found) return true;
    return execute(database, "ALTER TABLE project_task_states ADD COLUMN execution_completed "
                             "INTEGER NOT NULL DEFAULT 0 CHECK(execution_completed IN (0,1));", error);
}

bool ensureValidationPassedColumn(sqlite3* database, std::string& error) {
    bool found = false;
    if (!hasTableColumn(database, "project_task_states", "validation_passed", found, error)) {
        return false;
    }
    if (found) return true;
    return execute(database, "ALTER TABLE project_task_states ADD COLUMN validation_passed "
                             "INTEGER NOT NULL DEFAULT 0 CHECK(validation_passed IN (0,1));", error);
}

bool ensureTaskStateSchema(sqlite3* database, std::string& error) {
    bool hasResumeState = false;
    if (!hasTableColumn(database, "project_task_states", "resume_state", hasResumeState, error)) {
        return false;
    }
    if (hasResumeState) return true;

    if (!execute(database, "BEGIN IMMEDIATE;", error)) return false;
    bool success = execute(database, "DROP TABLE IF EXISTS project_task_states_v2;", error) &&
        execute(database,
            "CREATE TABLE project_task_states_v2 ("
            "chat_id INTEGER PRIMARY KEY REFERENCES chats(id),"
            "state TEXT NOT NULL DEFAULT 'PLANNING' "
                "CHECK(state IN ('PLANNING','EXECUTION','VALIDATION','DONE','PAUSED')),"
            "resume_state TEXT DEFAULT NULL "
                "CHECK(resume_state IS NULL OR resume_state IN "
                      "('PLANNING','EXECUTION','VALIDATION','DONE')),"
            "plan TEXT NOT NULL DEFAULT '',"
            "validation_report TEXT NOT NULL DEFAULT '',"
            "execution_completed INTEGER NOT NULL DEFAULT 0 "
                "CHECK(execution_completed IN (0,1)),"
            "validation_passed INTEGER NOT NULL DEFAULT 0 "
                "CHECK(validation_passed IN (0,1)),"
            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "CHECK((state='PAUSED' AND resume_state IS NOT NULL) OR "
                  "(state<>'PAUSED' AND resume_state IS NULL))"
            ");", error) &&
        execute(database,
            "INSERT INTO project_task_states_v2("
                "chat_id, state, resume_state, plan, validation_report, "
                "execution_completed, validation_passed, updated_at) "
            "SELECT chat_id, "
                "CASE WHEN paused=1 THEN 'PAUSED' "
                     "WHEN state='planning' THEN 'PLANNING' "
                     "WHEN state='execution' THEN 'EXECUTION' "
                     "WHEN state='validation' THEN 'VALIDATION' "
                     "WHEN state IN ('PLANNING','EXECUTION','VALIDATION','DONE') THEN state "
                     "ELSE state END, "
                "CASE WHEN paused=1 THEN "
                    "CASE WHEN state='planning' THEN 'PLANNING' "
                         "WHEN state='execution' THEN 'EXECUTION' "
                         "WHEN state='validation' THEN 'VALIDATION' "
                         "WHEN state IN ('PLANNING','EXECUTION','VALIDATION','DONE') THEN state "
                         "ELSE state END "
                    "ELSE NULL END, "
                "plan, validation_report, execution_completed, 0, updated_at "
            "FROM project_task_states;", error) &&
        execute(database, "DROP TABLE project_task_states;", error) &&
        execute(database,
                "ALTER TABLE project_task_states_v2 RENAME TO project_task_states;", error);

    if (success && execute(database, "COMMIT;", error)) return true;
    std::string ignoredRollbackError;
    execute(database, "ROLLBACK;", ignoredRollbackError);
    return false;
}

bool ensureTransitionLogSchema(sqlite3* database, std::string& error) {
    Statement statement(nullptr, sqlite3_finalize);
    if (!prepare(database,
                 "SELECT sql FROM sqlite_master WHERE type='table' AND name='task_state_transition_log';",
                 statement, error)) return false;
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return true;
    if (result != SQLITE_ROW) {
        error = sqlite3_errmsg(database);
        return false;
    }
    const std::string schema = columnText(statement.get(), 0);
    if (schema.find("EXECUTION_RESULT_READY") != std::string::npos &&
        schema.find("VALIDATION_RESULT_READY") != std::string::npos) return true;
    statement.reset();

    if (!execute(database, "BEGIN IMMEDIATE;", error)) return false;
    bool success = execute(database, "DROP TABLE IF EXISTS task_state_transition_log_v2;", error) &&
        execute(database,
            "CREATE TABLE task_state_transition_log_v2 ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "chat_id INTEGER NOT NULL REFERENCES chats(id) ON DELETE CASCADE,"
            "previous_state TEXT DEFAULT NULL CHECK(previous_state IS NULL OR previous_state IN "
                "('PLANNING','EXECUTION','VALIDATION','DONE','PAUSED')),"
            "action TEXT NOT NULL CHECK(action IN "
                "('CREATE_TASK','APPROVE_PLAN','REGENERATE_PLAN','EXECUTION_FINISHED',"
                 "'VALIDATION_PASSED','VALIDATION_FAILED','EXECUTION_RESULT_READY',"
                 "'VALIDATION_RESULT_READY','PAUSE','RESUME')),"
            "new_state TEXT NOT NULL CHECK(new_state IN "
                "('PLANNING','EXECUTION','VALIDATION','DONE','PAUSED')),"
            "timestamp TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
            ");", error) &&
        execute(database,
            "INSERT INTO task_state_transition_log_v2"
            "(id,chat_id,previous_state,action,new_state,timestamp) "
            "SELECT id,chat_id,previous_state,action,new_state,timestamp "
            "FROM task_state_transition_log;", error) &&
        execute(database, "DROP TABLE task_state_transition_log;", error) &&
        execute(database,
                "ALTER TABLE task_state_transition_log_v2 RENAME TO task_state_transition_log;",
                error) &&
        execute(database,
                "CREATE INDEX IF NOT EXISTS idx_task_state_transition_log_chat_id_id "
                "ON task_state_transition_log(chat_id, id);", error);
    if (success && execute(database, "COMMIT;", error)) return true;
    std::string ignoredRollbackError;
    execute(database, "ROLLBACK;", ignoredRollbackError);
    return false;
}

bool insertTransitionLog(sqlite3* database, const std::string& chatId,
                         const std::string& previousState, const std::string& action,
                         const std::string& newState, std::string& error) {
    Statement statement(nullptr, sqlite3_finalize);
    const char* sql =
        "INSERT INTO task_state_transition_log("
            "chat_id, previous_state, action, new_state, timestamp) "
        "VALUES(?1, ?2, ?3, ?4, CURRENT_TIMESTAMP);";
    return prepare(database, sql, statement, error) &&
           bindChatId(database, statement.get(), 1, chatId, error) &&
           bindNullableText(database, statement.get(), 2, previousState, error) &&
           bindText(database, statement.get(), 3, action, error) &&
           bindText(database, statement.get(), 4, newState, error) &&
           stepDone(database, statement.get(), error);
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
    if (!execute(database_, kCreateTableSql, initializationError_) ||
        !execute(database_, kCreateTaskStateTableSql, initializationError_) ||
        !ensureExecutionCompletedColumn(database_, initializationError_) ||
        !ensureTaskStateSchema(database_, initializationError_) ||
        !ensureValidationPassedColumn(database_, initializationError_) ||
        !execute(database_, kCreateTransitionLogSql, initializationError_) ||
        !ensureTransitionLogSchema(database_, initializationError_)) {
        return;
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
    if (success) {
        success = insertTransitionLog(database_, id, {}, "CREATE_TASK", "PLANNING", error);
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
    bool taskStateCreated = false;
    if (success) {
        Statement statement(nullptr, sqlite3_finalize);
        success = prepare(database_, "INSERT OR IGNORE INTO project_task_states(chat_id) VALUES(?1);",
                          statement, error) &&
                  bindChatId(database_, statement.get(), 1, chatId, error) &&
                  stepDone(database_, statement.get(), error);
        if (success) taskStateCreated = sqlite3_changes(database_) == 1;
    }
    if (success && taskStateCreated) {
        success = insertTransitionLog(database_, chatId, {}, "CREATE_TASK", "PLANNING", error);
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

bool MemoryStore::loadTaskState(const std::string& chatId, ProjectTaskState& taskState,
                                std::string& error) const {
    taskState = ProjectTaskState{};
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    Statement statement(nullptr, sqlite3_finalize);
    if (!prepare(database_, "SELECT state, resume_state, plan, validation_report, execution_completed, "
                            "validation_passed "
                            "FROM project_task_states WHERE chat_id=?1;", statement, error) ||
        !bindChatId(database_, statement.get(), 1, chatId, error)) return false;
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return true;
    if (result == SQLITE_ROW) {
        taskState.state = columnText(statement.get(), 0);
        taskState.resumeState = columnText(statement.get(), 1);
        taskState.plan = columnText(statement.get(), 2);
        taskState.validationReport = columnText(statement.get(), 3);
        taskState.executionCompleted = sqlite3_column_int(statement.get(), 4) != 0;
        taskState.validationPassed = sqlite3_column_int(statement.get(), 5) != 0;
        if (!isTaskState(taskState.state) ||
            (taskState.state == "PAUSED"
                ? !isResumableTaskState(taskState.resumeState)
                : !taskState.resumeState.empty())) {
            taskState = ProjectTaskState{};
            error = "Invalid persisted task state";
            return false;
        }
        if (sqlite3_step(statement.get()) == SQLITE_DONE) return true;
        taskState = ProjectTaskState{};
        error = sqlite3_errmsg(database_);
        return false;
    }
    error = sqlite3_errmsg(database_);
    return false;
}

bool MemoryStore::commitTaskTransition(const std::string& chatId,
                                       const std::string& expectedState,
                                       const std::string& action,
                                       const ProjectTaskState& nextState,
                                       const std::string* pauseSummary,
                                       std::string& error) {
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    if (!isTaskState(expectedState) || !isTaskState(nextState.state) ||
        !isAllowedTaskTransition(expectedState, action, nextState.state)) {
        error = "Invalid task transition: " + expectedState + " --" + action + "--> " +
                nextState.state;
        return false;
    }
    if (action == "PAUSE") {
        if (!pauseSummary || nextState.resumeState != expectedState) {
            error = "Pause requires a summary and the exact state to resume";
            return false;
        }
    } else if (pauseSummary) {
        error = "A pause summary is allowed only for PAUSE";
        return false;
    }
    if (nextState.state != "PAUSED" && !nextState.resumeState.empty()) {
        error = "Only PAUSED can contain a resume state";
        return false;
    }

    if (!execute(database_, "BEGIN IMMEDIATE;", error)) return false;
    bool success = true;
    std::string storedState;
    std::string storedResumeState;
    {
        Statement statement(nullptr, sqlite3_finalize);
        if (!prepare(database_, "SELECT state, resume_state FROM project_task_states "
                                "WHERE chat_id=?1;", statement, error) ||
            !bindChatId(database_, statement.get(), 1, chatId, error)) {
            success = false;
        } else {
            const int result = sqlite3_step(statement.get());
            if (result == SQLITE_ROW) {
                storedState = columnText(statement.get(), 0);
                storedResumeState = columnText(statement.get(), 1);
                if (sqlite3_step(statement.get()) != SQLITE_DONE) {
                    error = sqlite3_errmsg(database_);
                    success = false;
                }
            } else {
                error = result == SQLITE_DONE ? "Task state not found" : sqlite3_errmsg(database_);
                success = false;
            }
        }
    }
    if (success && storedState != expectedState) {
        error = "Task state changed concurrently: expected " + expectedState +
                ", current " + storedState;
        success = false;
    }
    if (success && action == "RESUME" && storedResumeState != nextState.state) {
        error = "Resume must return to the state saved by PAUSE";
        success = false;
    }

    if (success && pauseSummary) {
        Statement statement(nullptr, sqlite3_finalize);
        const char* sql =
            "INSERT INTO project_summaries(chat_id, summary, updated_at) "
            "VALUES(?1, ?2, CURRENT_TIMESTAMP) "
            "ON CONFLICT(chat_id) DO UPDATE SET summary=excluded.summary, "
            "updated_at=CURRENT_TIMESTAMP;";
        success = prepare(database_, sql, statement, error) &&
                  bindChatId(database_, statement.get(), 1, chatId, error) &&
                  bindText(database_, statement.get(), 2, *pauseSummary, error) &&
                  stepDone(database_, statement.get(), error);
    }

    if (success) {
        Statement statement(nullptr, sqlite3_finalize);
        const char* sql =
            "UPDATE project_task_states SET state=?2, resume_state=?3, plan=?4, "
            "validation_report=?5, execution_completed=?6, validation_passed=?7, "
            "updated_at=CURRENT_TIMESTAMP "
            "WHERE chat_id=?1 AND state=?8;";
        success = prepare(database_, sql, statement, error) &&
                  bindChatId(database_, statement.get(), 1, chatId, error) &&
                  bindText(database_, statement.get(), 2, nextState.state, error) &&
                  bindNullableText(database_, statement.get(), 3, nextState.resumeState, error) &&
                  bindText(database_, statement.get(), 4, nextState.plan, error) &&
                  bindText(database_, statement.get(), 5, nextState.validationReport, error) &&
                  bindInt(database_, statement.get(), 6,
                          nextState.executionCompleted ? 1 : 0, error) &&
                  bindInt(database_, statement.get(), 7,
                          nextState.validationPassed ? 1 : 0, error) &&
                  bindText(database_, statement.get(), 8, expectedState, error) &&
                  stepDone(database_, statement.get(), error);
        if (success && sqlite3_changes(database_) != 1) {
            error = "Task state changed concurrently";
            success = false;
        }
    }
    if (success) {
        success = insertTransitionLog(database_, chatId, expectedState, action,
                                      nextState.state, error);
    }
    if (success && execute(database_, "COMMIT;", error)) return true;
    std::string ignoredRollbackError;
    execute(database_, "ROLLBACK;", ignoredRollbackError);
    return false;
}

bool MemoryStore::loadTaskTransitionLog(
        const std::string& chatId, std::vector<TaskTransitionLog>& transitions,
        std::string& error) const {
    transitions.clear();
    error.clear();
    if (!isReady()) { error = initializationError_; return false; }
    Statement statement(nullptr, sqlite3_finalize);
    const char* sql =
        "SELECT previous_state, action, new_state, timestamp "
        "FROM task_state_transition_log WHERE chat_id=?1 ORDER BY id;";
    if (!prepare(database_, sql, statement, error) ||
        !bindChatId(database_, statement.get(), 1, chatId, error)) {
        return false;
    }
    int result = SQLITE_OK;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
        transitions.push_back({columnText(statement.get(), 0), columnText(statement.get(), 1),
                               columnText(statement.get(), 2), columnText(statement.get(), 3)});
    }
    if (result == SQLITE_DONE) return true;
    transitions.clear();
    error = sqlite3_errmsg(database_);
    return false;
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
        success = prepare(database_, "DELETE FROM task_state_transition_log WHERE chat_id=?1;",
                          statement, error) &&
                  bindChatId(database_, statement.get(), 1, chatId, error) &&
                  stepDone(database_, statement.get(), error);
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

    if (success && !replacementId.empty()) {
        success = insertTransitionLog(database_, replacementId, {}, "CREATE_TASK", "PLANNING",
                                      error);
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
