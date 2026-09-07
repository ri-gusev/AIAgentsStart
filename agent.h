#pragma once

#include "api_client.h"

#include <cstddef>
#include <string>
#include <vector>

class Agent {
public:
    explicit Agent(const std::string& configPath = "agent_config.local.json");

    bool isReady() const;
    const std::string& initializationError() const;
    const std::string& modelName() const;

    // Sends one user turn to the model and keeps only the configured sliding window in memory.
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
    };

    bool loadConfig(const std::string& configPath, std::string& error);
    std::string buildConversation(const std::string& userMessage) const;
    std::string buildReviewConversation(const std::string& userMessage,
                                        const std::string& draft) const;
    bool reviewAnswer(const std::string& userMessage, const std::string& draft,
                      std::string& finalAnswer, std::string& error) const;
    void remember(const std::string& userMessage, const std::string& answer);

    Config config_;
    std::string apiKey_;
    ApiClient apiClient_;
    std::string initializationError_;
    std::vector<DialogTurn> shortTermMemory_;
};
