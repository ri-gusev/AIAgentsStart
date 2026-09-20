#pragma once

#include <string>
#include <vector>

struct sqlite3;

// Project invariants deliberately have their own database and lifecycle. They
// are not facts and must never be written by automatic memory routing.
struct ProjectInvariant {
    std::string key;
    std::string value;
    std::string description;
};

class InvariantStore {
public:
    explicit InvariantStore(const std::string& databasePath);
    ~InvariantStore();

    InvariantStore(const InvariantStore&) = delete;
    InvariantStore& operator=(const InvariantStore&) = delete;

    bool isReady() const;
    const std::string& initializationError() const;
    bool load(const std::string& projectId, std::vector<ProjectInvariant>& invariants,
              std::string& error) const;
    bool create(const std::string& projectId, const ProjectInvariant& invariant,
                std::string& error);
    bool update(const std::string& projectId, const std::string& currentKey,
                const ProjectInvariant& invariant, std::string& error);
    bool remove(const std::string& projectId, const std::string& key, std::string& error);
    bool removeProject(const std::string& projectId, std::string& error);

private:
    sqlite3* database_ = nullptr;
    std::string initializationError_;
};
