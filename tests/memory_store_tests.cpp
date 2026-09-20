#include "memory_store.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct TempDatabase {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("agent-memory-store-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");

    ~TempDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
};

void executeSql(sqlite3* database, const char* sql) {
    char* rawError = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &rawError);
    const std::string error = rawError ? rawError : "SQLite statement failed";
    sqlite3_free(rawError);
    require(result == SQLITE_OK, error);
}

void testTransitionsPauseResumeAndLog() {
    TempDatabase temp;
    MemoryStore store(temp.path.string());
    require(store.isReady(), store.initializationError());

    std::string chatId, error;
    require(store.createChat("FSM", chatId, error), error);
    ProjectTaskState state;
    require(store.loadTaskState(chatId, state, error), error);
    require(state.state == "PLANNING" && state.resumeState.empty(),
            "A new task is not in PLANNING");

    std::vector<TaskTransitionLog> log;
    require(store.loadTaskTransitionLog(chatId, log, error), error);
    require(log.size() == 1 && log[0].previousState.empty() &&
            log[0].action == "CREATE_TASK" && log[0].newState == "PLANNING" &&
            !log[0].timestamp.empty(), "CREATE_TASK was not logged");

    state.state = "EXECUTION";
    state.plan = "Approved plan";
    require(store.commitTaskTransition(chatId, "PLANNING", "APPROVE_PLAN", state,
                                       nullptr, error), error);

    ProjectTaskState forbidden = state;
    forbidden.state = "DONE";
    require(!store.commitTaskTransition(chatId, "EXECUTION", "VALIDATION_PASSED",
                                        forbidden, nullptr, error),
            "A forbidden EXECUTION -> DONE transition was accepted");
    require(store.loadTaskState(chatId, state, error) && state.state == "EXECUTION",
            "A forbidden transition changed persisted state");

    ProjectTaskState paused = state;
    paused.state = "PAUSED";
    paused.resumeState = "EXECUTION";
    const std::string summary = "Paused execution summary";
    require(store.commitTaskTransition(chatId, "EXECUTION", "PAUSE", paused,
                                       &summary, error), error);
    std::string storedSummary;
    require(store.loadTaskState(chatId, state, error) && state.state == "PAUSED" &&
            state.resumeState == "EXECUTION" &&
            store.loadProjectSummary(chatId, storedSummary, error) &&
            storedSummary == summary, "PAUSE did not persist state and summary");

    ProjectTaskState resumed = state;
    resumed.state = "EXECUTION";
    resumed.resumeState.clear();
    require(store.commitTaskTransition(chatId, "PAUSED", "RESUME", resumed,
                                       nullptr, error), error);
    require(store.loadTaskTransitionLog(chatId, log, error) && log.size() == 4 &&
            log[1].action == "APPROVE_PLAN" && log[2].action == "PAUSE" &&
            log[3].action == "RESUME", "Transition log order is incorrect");
}

void testLegacyMigration() {
    TempDatabase temp;
    sqlite3* rawDatabase = nullptr;
    require(sqlite3_open(temp.path.string().c_str(), &rawDatabase) == SQLITE_OK,
            "Could not create legacy database");
    executeSql(rawDatabase,
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE chats(id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL);"
        "INSERT INTO chats(name) VALUES('Paused legacy'),('Active legacy');"
        "CREATE TABLE project_task_states("
            "chat_id INTEGER PRIMARY KEY REFERENCES chats(id),"
            "state TEXT NOT NULL CHECK(state IN ('planning','execution','validation','DONE')),"
            "plan TEXT NOT NULL DEFAULT '', validation_report TEXT NOT NULL DEFAULT '',"
            "paused INTEGER NOT NULL DEFAULT 0 CHECK(paused IN (0,1)),"
            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "execution_completed INTEGER NOT NULL DEFAULT 0 CHECK(execution_completed IN (0,1)));"
        "INSERT INTO project_task_states(chat_id,state,plan,validation_report,paused,execution_completed) "
            "VALUES(1,'execution','legacy plan','legacy report',1,1),"
                  "(2,'validation','active plan','active report',0,1);"
        "CREATE TABLE project_summaries("
            "chat_id INTEGER PRIMARY KEY REFERENCES chats(id), summary TEXT NOT NULL DEFAULT '',"
            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "INSERT INTO project_summaries(chat_id) VALUES(1),(2);"
        "CREATE TABLE long_term_memory(memory_key TEXT PRIMARY KEY NOT NULL,"
            "memory_value TEXT NOT NULL, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE working_memory(chat_id INTEGER NOT NULL REFERENCES chats(id),"
            "memory_key TEXT NOT NULL,memory_value TEXT NOT NULL,"
            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,PRIMARY KEY(chat_id,memory_key));"
        "CREATE TABLE agent_settings(setting_key TEXT PRIMARY KEY NOT NULL,setting_value TEXT NOT NULL);");
    sqlite3_close(rawDatabase);

    MemoryStore store(temp.path.string());
    require(store.isReady(), store.initializationError());
    std::string error;
    ProjectTaskState paused;
    ProjectTaskState active;
    require(store.loadTaskState("1", paused, error) && paused.state == "PAUSED" &&
            paused.resumeState == "EXECUTION" && paused.plan == "legacy plan" &&
            paused.validationReport == "legacy report" && paused.executionCompleted,
            "Legacy paused state was not migrated without data loss");
    require(store.loadTaskState("2", active, error) && active.state == "VALIDATION" &&
            active.resumeState.empty() && active.executionCompleted,
            "Legacy active state was not migrated to uppercase");

    std::vector<TaskTransitionLog> log;
    require(store.loadTaskTransitionLog("1", log, error) && log.empty(),
            "Migration fabricated historical transitions");
}

void testTransitionRollbackAndDeleteCleanup() {
    TempDatabase temp;
    MemoryStore store(temp.path.string());
    require(store.isReady(), store.initializationError());
    std::string chatId, otherChatId, error;
    require(store.createChat("Rollback", chatId, error) &&
            store.createChat("Other", otherChatId, error), error);

    sqlite3* rawDatabase = nullptr;
    require(sqlite3_open(temp.path.string().c_str(), &rawDatabase) == SQLITE_OK,
            "Could not open test database");
    executeSql(rawDatabase,
        "CREATE TRIGGER reject_pause_log BEFORE INSERT ON task_state_transition_log "
        "WHEN NEW.action='PAUSE' BEGIN SELECT RAISE(ABORT,'blocked transition log'); END;");
    sqlite3_close(rawDatabase);

    ProjectTaskState paused;
    paused.state = "PAUSED";
    paused.resumeState = "PLANNING";
    const std::string summary = "must roll back";
    require(!store.commitTaskTransition(chatId, "PLANNING", "PAUSE", paused,
                                        &summary, error),
            "Trigger-protected PAUSE unexpectedly succeeded");
    ProjectTaskState stored;
    std::string storedSummary;
    require(store.loadTaskState(chatId, stored, error) && stored.state == "PLANNING" &&
            store.loadProjectSummary(chatId, storedSummary, error) && storedSummary.empty(),
            "Failed transition did not roll back state and summary");

    std::string replacementId;
    require(store.deleteChat(chatId, replacementId, error) && replacementId.empty(), error);
    std::vector<TaskTransitionLog> log;
    require(store.loadTaskTransitionLog(chatId, log, error) && log.empty(),
            "Deleting a chat did not delete its transition log");
    require(store.loadTaskTransitionLog(otherChatId, log, error) && log.size() == 1 &&
            log[0].action == "CREATE_TASK", "Deleting a chat damaged another chat's log");
}
}

int main() {
    try {
        testTransitionsPauseResumeAndLog();
        testLegacyMigration();
        testTransitionRollbackAndDeleteCleanup();
        std::cout << "memory_store_tests: passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "memory_store_tests: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
