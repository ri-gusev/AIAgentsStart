#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ApiToolCall {
    std::string id, name, argumentsJson;
};

struct ApiChatResponse {
    std::string content;
    std::string assistantMessageJson;
    std::vector<ApiToolCall> toolCalls;
};

struct ApiTokenUsage {
    std::uint64_t inputTokens = 0;
    std::uint64_t cachedInputTokens = 0;
    std::uint64_t outputTokens = 0;
    std::uint64_t totalTokens = 0;
};

class ApiClient {
public:
    ApiClient();
    ~ApiClient();

    ApiClient(const ApiClient&) = delete;
    ApiClient& operator=(const ApiClient&) = delete;

    bool isReady() const;
    bool sendChatCompletion(const std::string& apiKey, const std::string& model,
                            const std::string& messagesJson, std::string& answer,
                            std::string& error, bool jsonResponse = false,
                            ApiTokenUsage* usage = nullptr,
                            std::string* finishReason = nullptr,
                            const std::string& toolsJson = {},
                            ApiChatResponse* completion = nullptr) const;
    static bool parseChatCompletionResponse(const std::string& responseJson,
                                            ApiChatResponse& completion, std::string& error);

private:
    bool initialized_ = false;
};
