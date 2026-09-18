#pragma once

#include <string>
#include <vector>

struct sqlite3;

struct LongTermMemoryFact {
    std::string key;
    std::string value;
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

private:
    sqlite3* database_ = nullptr;
    std::string initializationError_;
};
