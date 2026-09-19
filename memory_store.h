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
    std::string state = "planning";
    std::string plan;
    std::string validationReport;
    bool paused = false;
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
    bool saveProjectPause(const std::string& chatId, const std::string& summary,
                          const ProjectTaskState& taskState, std::string& error);
    bool loadTaskState(const std::string& chatId, ProjectTaskState& taskState,
                       std::string& error) const;
    bool saveTaskState(const std::string& chatId, const ProjectTaskState& taskState,
                       std::string& error);
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
