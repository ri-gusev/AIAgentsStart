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

    struct ChatMessage {
        std::string role;
        std::string content;
    };

    explicit Agent(const std::string& configPath = "agent_config.local.json");
    ~Agent();

    bool isReady() const;
    const std::string& initializationError() const;
    const std::string& modelName() const;
    std::size_t rawHistoryMessageCount() const;
    std::size_t rawMessageLimit() const;
    std::size_t pendingSummaryMessageCount() const;
    std::size_t longTermFactCount() const;
    std::size_t completedRequestCount() const;
    std::size_t summaryEveryRequests() const;
    bool hasConversationSummary() const;
    bool inputRejected() const;
    bool inputSuspicious() const;
    const std::vector<std::string>& warnings() const;
    const std::vector<ChatMessage>& visibleConversation() const;
    const TokenStatistics& tokenStatistics() const;
    CostStatistics costStatistics() const;

    bool respond(const std::string& userMessage, std::string& answer, std::string& error);

private:
    struct Config {
        std::string model;
        std::string baseInstruction;
        std::string inputPolicy;
        std::string outputPolicy;
        std::size_t shortTermMemoryMessages = 5;
        std::size_t summaryEveryRequests = 5;
        std::string longTermMemoryDatabase = "agent_memory.db";
        double inputPricePerMillion = 0.0;
        double cachedInputPricePerMillion = 0.0;
        double outputPricePerMillion = 0.0;
    };

    bool loadConfig(const std::string& configPath, std::string& error);
    bool applyInputPolicy(const std::string& userMessage, std::string& error);
    bool containsSecret(const std::string& text) const;
    bool reloadLongTermMemory(std::string& error);
    bool updateLongTermMemory(const std::string& userMessage,
                              const std::string& answer, std::string& error);
    bool summarizeHistory(std::string& error);
    bool reviewAnswer(const std::string& userMessage, const std::string& draft,
                      std::string& finalAnswer, std::string& error);
    std::string buildLongTermMemoryPrompt() const;
    std::string buildMemoryDecisionConversation(const std::string& userMessage,
                                                const std::string& answer) const;
    std::string buildConversation(const std::string& userMessage) const;
    std::string buildReviewConversation(const std::string& userMessage,
                                        const std::string& draft) const;
    std::string buildSummaryConversation(std::size_t messageCount) const;
    void appendContext(std::string& messages, bool& first) const;
    bool sendTrackedChatCompletion(const std::string& messagesJson, std::string& answer,
                                   std::string& error, bool jsonResponse,
                                   bool summaryRequest = false,
                                   std::string* finishReason = nullptr);
    void remember(const std::string& userMessage, const std::string& answer);

    Config config_;
    std::string apiKey_;
    ApiClient apiClient_;
    std::unique_ptr<MemoryStore> memoryStore_;
    std::string initializationError_;
    std::vector<ChatMessage> rawHistory_;
    std::vector<ChatMessage> sessionTranscript_;
    std::vector<LongTermMemoryFact> longTermMemory_;
    std::string conversationSummary_;
    std::size_t completedRequests_ = 0;
    bool summaryRetryPending_ = false;
    bool inputRejected_ = false;
    bool inputSuspicious_ = false;
    std::vector<std::string> warnings_;
    TokenStatistics tokenStatistics_;
};
