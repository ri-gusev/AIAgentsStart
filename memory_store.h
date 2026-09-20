#pragma once

#include <string>
#include <vector>

struct sqlite3;

struct LongTermMemoryFact {
    std::string key;
    std::string value;
};

struct StoredChat {
    std::string id;
    std::string name;
};

struct ProjectTaskState {
    std::string state = "PLANNING";
    std::string resumeState;
    std::string plan;
    std::string validationReport;
    bool executionCompleted = false;
};

struct TaskTransitionLog {
    std::string previousState;
    std::string action;
    std::string newState;
    std::string timestamp;
};

class MemoryStore {
public:
    explicit MemoryStore(const std::string& databasePath);
    ~MemoryStore();

    MemoryStore(const MemoryStore&) = delete;
    MemoryStore& operator=(const MemoryStore&) = delete;

    bool isReady() const;
    const std::string& initializationError() const;
    bool loadAll(std::vector<LongTermMemoryFact>& facts, std::string& error) const;
    bool upsert(const LongTermMemoryFact& fact, std::string& error);
    bool loadChats(std::vector<StoredChat>& chats, std::string& error) const;
    bool createChat(const std::string& name, std::string& id, std::string& error);
    bool ensureProjectData(const std::string& chatId, std::string& error);
    bool deleteChat(const std::string& chatId, std::string& replacementId,
                    std::string& error);
    bool loadProjectSummary(const std::string& chatId, std::string& summary,
                            std::string& error) const;
    bool loadTaskState(const std::string& chatId, ProjectTaskState& taskState,
                       std::string& error) const;
    bool commitTaskTransition(const std::string& chatId, const std::string& expectedState,
                              const std::string& action, const ProjectTaskState& nextState,
                              const std::string* pauseSummary, std::string& error);
    bool loadTaskTransitionLog(const std::string& chatId,
                               std::vector<TaskTransitionLog>& transitions,
                               std::string& error) const;
    bool loadWorking(const std::string& chatId, std::vector<LongTermMemoryFact>& facts,
                     std::string& error) const;
    bool upsertWorking(const std::string& chatId, const LongTermMemoryFact& fact,
                       std::string& error);
    bool loadSetting(const std::string& key, std::string& value, std::string& error) const;
    bool saveSetting(const std::string& key, const std::string& value, std::string& error);
    bool saveFacts(const std::string& chatId,
                   const std::vector<LongTermMemoryFact>& working,
                   const std::vector<LongTermMemoryFact>& longTerm, std::string& error);

private:
    sqlite3* database_ = nullptr;
    std::string initializationError_;
};
