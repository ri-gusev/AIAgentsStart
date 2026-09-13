#pragma once

#include <cstdint>
#include <string>

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
                            std::string* finishReason = nullptr) const;

private:
    bool initialized_ = false;
};
