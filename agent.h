#pragma once

#include "api_client.h"
#include "memory_store.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Agent {
public:
    struct TokenStatistics {
        std::uint64_t inputTokens = 0;
        std::uint64_t cachedInputTokens = 0;
        std::uint64_t outputTokens = 0;
        std::uint64_t totalTokens = 0;
    };

    struct CostStatistics {
        double inputUsd = 0.0;
        double outputUsd = 0.0;
        double totalUsd = 0.0;
    };

    struct ChatMessage {
        std::string role;
        std::string content;
    };

    struct BranchInfo {
        std::size_t id = 0;
        std::string label;
        bool active = false;
    };

    explicit Agent(const std::string& configPath = "agent_config.local.json");
    ~Agent();

    bool isReady() const;
    const std::string& initializationError() const;
    const std::string& modelName() const;
    int strategy() const;
    bool setStrategy(int strategy, std::string& error);
    bool selectBranch(std::size_t branchId, std::string& error);
    bool resetAllMemory(std::string& error);
    std::size_t rawHistoryMessageCount() const;
    std::size_t longTermFactCount() const;
    std::size_t activeBranchId() const;
    std::vector<BranchInfo> branches() const;
    std::vector<ChatMessage> visibleConversation() const;
    const TokenStatistics& tokenStatistics() const;
    CostStatistics costStatistics() const;

    bool respond(const std::string& userMessage, std::string& answer, std::string& error);

private:
    struct DialogTurn {
        std::string user;
        std::string assistant;
    };

    struct Branch {
        std::size_t id = 0;
        std::size_t parentId = 0;
        std::string label;
        std::string direction;
        std::string checkpoint;
        std::vector<DialogTurn> checkpointContext;
        std::vector<DialogTurn> history;
    };

    struct Config {
        std::string model;
        std::string inputPolicy;
        std::string outputPolicy;
        std::size_t shortTermMemoryMessages = 10;
        std::string longTermMemoryDatabase = "agent_memory.db";
        double inputPricePerMillion = 0.0;
        double cachedInputPricePerMillion = 0.0;
        double outputPricePerMillion = 0.0;
    };

    bool loadConfig(const std::string& configPath, std::string& error);
    bool reloadLongTermMemory(std::string& error);
    bool updateLongTermMemory(const std::string& userMessage, std::string& error);
    std::string buildLongTermMemoryPrompt() const;
    std::string buildMemoryDecisionConversation(const std::string& userMessage) const;
    std::string buildConversation(const std::string& userMessage) const;
    std::string buildReviewConversation(const std::string& userMessage,
                                        const std::string& draft) const;
    std::string buildBranchDetectionConversation(const std::string& userMessage) const;
    bool detectAndCreateBranches(const std::string& userMessage,
                                 std::string& answer, bool& checkpointCreated,
                                 std::string& error);
    bool sendTrackedChatCompletion(const std::string& messagesJson, std::string& answer,
                                   std::string& error, bool jsonResponse,
                                   std::string* finishReason = nullptr);
    bool reviewAnswer(const std::string& userMessage, const std::string& draft,
                      std::string& finalAnswer, std::string& error);
    void appendActiveContext(std::string& messages, bool& first) const;
    void remember(const std::string& userMessage, const std::string& answer);
    void trimHistory(std::vector<DialogTurn>& history) const;
    std::vector<DialogTurn> activeContextSnapshot() const;
    std::vector<DialogTurn>& activeHistory();
    const std::vector<DialogTurn>& activeHistory() const;
    Branch* findBranch(std::size_t id);
    const Branch* findBranch(std::size_t id) const;

    Config config_;
    std::string apiKey_;
    ApiClient apiClient_;
    std::unique_ptr<MemoryStore> memoryStore_;
    std::string initializationError_;
    int strategy_ = 1;
    std::vector<DialogTurn> shortTermOnlyHistory_;
    std::vector<DialogTurn> sqliteHistory_;
    std::vector<LongTermMemoryFact> longTermMemory_;
    std::vector<Branch> branches_;
    std::size_t activeBranchId_ = 0;
    std::size_t nextBranchId_ = 1;
    TokenStatistics tokenStatistics_;
};
