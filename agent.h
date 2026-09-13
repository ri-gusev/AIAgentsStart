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
        std::uint64_t summaryInputTokens = 0;
        std::uint64_t summaryCachedInputTokens = 0;
        std::uint64_t summaryOutputTokens = 0;
        std::uint64_t summaryTotalTokens = 0;
    };

    struct CostStatistics {
        double inputUsd = 0.0;
        double outputUsd = 0.0;
        double totalUsd = 0.0;
        double summaryUsd = 0.0;
    };

    explicit Agent(const std::string& configPath = "agent_config.local.json");
    ~Agent();

    bool isReady() const;
    const std::string& initializationError() const;
    const std::string& modelName() const;
    bool compressionEnabled() const;
    void setCompressionEnabled(bool enabled);
    std::size_t rawHistoryTurnCount() const;
    bool hasConversationSummary() const;
    const TokenStatistics& tokenStatistics() const;
    CostStatistics costStatistics() const;

    // Sends one user turn to the model and maintains raw history plus an optional summary.
    bool respond(const std::string& userMessage, std::string& answer, std::string& error);

private:
    struct DialogTurn {
        std::string user;
        std::string assistant;
    };

    struct Config {
        std::string model;
        std::string inputPolicy;
        std::string outputPolicy;
        std::size_t shortTermMemoryTurns = 15;
        bool compressionEnabled = true;
        std::size_t compressionKeepTurns = 10;
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
    std::string buildSummaryConversation(std::size_t turnCount) const;
    bool sendTrackedChatCompletion(const std::string& messagesJson, std::string& answer,
                                   std::string& error, bool jsonResponse,
                                   bool summaryRequest,
                                   std::string* finishReason = nullptr);
    bool compressHistoryIfNeeded(std::string& error);
    bool reviewAnswer(const std::string& userMessage, const std::string& draft,
                      std::string& finalAnswer, std::string& error);
    void remember(const std::string& userMessage, const std::string& answer);

    Config config_;
    std::string apiKey_;
    ApiClient apiClient_;
    std::unique_ptr<MemoryStore> memoryStore_;
    std::string initializationError_;
    std::string conversationSummary_;
    std::vector<DialogTurn> shortTermMemory_;
    std::vector<LongTermMemoryFact> longTermMemory_;
    TokenStatistics tokenStatistics_;
};
