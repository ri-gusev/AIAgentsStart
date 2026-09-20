#pragma once

#include "api_client.h"
#include "invariant_store.h"
#include "memory_store.h"

#include <cstddef>
#include <cstdint>
#include <map>
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
        std::string id;
        bool workingSaved = false;
        bool longTermSaved = false;
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

    const std::vector<StoredChat>& chats() const;
    const std::string& activeChatId() const;
    const std::string& activeChatName() const;
    const std::string& memoryMode() const;
    const std::string& personalization() const;
    const std::vector<LongTermMemoryFact>& workingMemoryFacts() const;
    const std::vector<LongTermMemoryFact>& longTermMemoryFacts() const;
    const std::vector<ProjectInvariant>& projectInvariants() const;
    const ProjectTaskState& taskState() const;
    const std::string& projectSummary() const;
    bool createChat(const std::string& name, std::string& error);
    bool deleteChat(const std::string& chatId, std::string& error);
    bool selectChat(const std::string& chatId, std::string& error);
    bool setMemoryMode(const std::string& mode, std::string& error);
    bool setPersonalization(const std::string& text, std::string& error);
    bool createInvariant(const std::string& projectId, const ProjectInvariant& invariant,
                         std::string& error);
    bool updateInvariant(const std::string& projectId, const std::string& currentKey,
                         const ProjectInvariant& invariant, std::string& error);
    bool deleteInvariant(const std::string& projectId, const std::string& key,
                         std::string& error);
    bool saveMessageToMemory(const std::string& chatId, const std::string& messageId,
                             const std::string& target, std::string& error);
    bool respondInChat(const std::string& chatId, const std::string& userMessage,
                       std::string& answer, std::string& error);
    bool handleChatMessage(const std::string& chatId, const std::string& userMessage,
                           std::string& answer, std::string& error);
    bool generateTaskPlan(const std::string& taskRequest, std::string& plan,
                          std::string& error);
    bool performTaskAction(const std::string& action, std::string& error);
    bool approveTaskPlan(std::string& error);
    bool reviseTaskPlan(const std::string& feedback, std::string& plan,
                        std::string& error);
    bool moveTaskToValidation(std::string& error);
    bool validateTask(bool& passed, std::string& error);
    bool returnTaskToExecution(std::string& error);
    bool pauseTask(std::string& error);
    bool resumeTask(std::string& error);

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
        std::string invariantsDatabase = "project_invariants.db";
        double inputPricePerMillion = 0.0;
        double cachedInputPricePerMillion = 0.0;
        double outputPricePerMillion = 0.0;
    };

    struct ChatState {
        std::vector<ChatMessage> rawHistory;
        std::vector<ChatMessage> transcript;
        std::vector<LongTermMemoryFact> workingFacts;
        std::vector<ProjectInvariant> invariants;
        std::string summary;
        std::string projectSummary;
        ProjectTaskState task;
        std::size_t completedRequests = 0;
        std::size_t nextMessageId = 1;
        bool summaryRetryPending = false;
        bool phaseRunning = false;
    };

    enum class TaskAction {
        CreateTask,
        ApprovePlan,
        RegeneratePlan,
        ExecutionFinished,
        ValidationPassed,
        ValidationFailed,
        ExecutionResultReady,
        ValidationResultReady,
        Pause,
        Resume
    };

    ChatState& activeChat();
    const ChatState& activeChat() const;
    void clearRequestStatus();
    std::string baseInstruction() const;
    std::string buildWorkingMemoryPrompt() const;
    std::string buildInvariantPrompt() const;

    bool loadConfig(const std::string& configPath, std::string& error);
    bool applyInputPolicy(const std::string& userMessage, std::string& error);
    bool containsSecret(const std::string& text) const;
    bool reloadLongTermMemory(std::string& error);
    bool reloadWorkingMemory(const std::string& chatId, std::string& error);
    bool reloadProjectData(const std::string& chatId, std::string& error);
    bool reloadInvariants(const std::string& chatId, std::string& error);
    bool validateInvariant(const ProjectInvariant& invariant, std::string& error) const;
    bool transitionTask(const std::string& taskId, TaskAction action, ProjectTaskState next,
                        std::string& error, const std::string* pauseSummary = nullptr);
    bool runValidationPhase(std::string& error);
    bool processUserMessage(const std::string& userMessage, std::string& answer,
                            std::string& error);
    bool generateOrdinaryAnswer(const std::string& userMessage, std::string& answer,
                                std::string& error);
    std::string buildTaskPlanConversation(const std::string& taskRequest) const;
    std::string buildValidationConversation() const;
    std::string buildProjectPauseConversation() const;
    bool generateExecutionAnswer(const std::string& changeRequest, std::string& answer,
                                 std::string& error);
    bool summarizeProjectOnPause(std::string& summary, std::string& error);
    bool updateAutomaticMemory(const std::string& userMessage,
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
    void completeAcceptedTurn(const std::string& userMessage, const std::string& answer);
    void appendAssistantEvent(const std::string& message);
    void remember(const std::string& userMessage, const std::string& answer);

    Config config_;
    std::string apiKey_;
    ApiClient apiClient_;
    std::unique_ptr<MemoryStore> memoryStore_;
    std::unique_ptr<InvariantStore> invariantStore_;
    std::string initializationError_;
    std::vector<StoredChat> chats_;
    std::map<std::string, ChatState> chatStates_;
    std::string activeChatId_;
    std::string sessionId_;
    std::string memoryMode_ = "auto";
    std::string personalization_;
    std::vector<LongTermMemoryFact> longTermMemory_;
    bool inputRejected_ = false;
    bool inputSuspicious_ = false;
    std::vector<std::string> warnings_;
    TokenStatistics tokenStatistics_;
};
