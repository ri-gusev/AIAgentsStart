#pragma once

#include <string>

class ApiClient {
public:
    ApiClient();
    ~ApiClient();

    ApiClient(const ApiClient&) = delete;
    ApiClient& operator=(const ApiClient&) = delete;

    bool isReady() const;
    bool sendChatCompletion(const std::string& apiKey, const std::string& model,
                            const std::string& messagesJson, std::string& answer,
                            std::string& error, bool jsonResponse = false) const;

private:
    bool initialized_ = false;
};
