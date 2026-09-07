#include "api_client.h"

#include <curl/curl.h>

#include <cctype>

namespace {
size_t writeResponse(void* data, size_t size, size_t count, void* userData) {
    static_cast<std::string*>(userData)->append(static_cast<char*>(data), size * count);
    return size * count;
}

std::string jsonEscape(const std::string& text) {
    std::string result;
    for (unsigned char c : text) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += c < 0x20 ? "?" : std::string(1, static_cast<char>(c));
        }
    }
    return result;
}

std::string extractContent(const std::string& json) {
    const auto message = json.find("\"message\"");
    if (message == std::string::npos) return {};
    const auto content = json.find("\"content\"", message);
    if (content == std::string::npos) return {};
    auto start = json.find(':', content + 9);
    if (start == std::string::npos) return {};
    do { ++start; } while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start])));
    if (start >= json.size() || json[start++] != '"') return {};
    std::string result;
    bool escaped = false;
    for (; start < json.size(); ++start) {
        const char c = json[start];
        if (escaped) {
            switch (c) {
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            default: result += c; break;
            }
            escaped = false;
        } else if (c == '\\') escaped = true;
        else if (c == '"') return result;
        else result += c;
    }
    return {};
}
}

ApiClient::ApiClient() : initialized_(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {}
ApiClient::~ApiClient() { if (initialized_) curl_global_cleanup(); }
bool ApiClient::isReady() const { return initialized_; }

bool ApiClient::sendChatCompletion(const std::string& apiKey, const std::string& model,
                                   const std::string& messagesJson, std::string& answer,
                                   std::string& error) const {
    if (!initialized_) { error = "Could not initialize libcurl"; return false; }
    const std::string body = "{\"model\":\"" + jsonEscape(model) + "\",\"messages\":" + messagesJson +
                             ",\"max_completion_tokens\":200}";
    CURL* curl = curl_easy_init();
    if (!curl) { error = "Could not initialize libcurl"; return false; }
    std::string response;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + apiKey).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/chat/completions");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    CURLcode result = CURLE_OK;
    for (int attempt = 0; attempt < 2; ++attempt) {
        response.clear();
        result = curl_easy_perform(curl);
        if (result != CURLE_OPERATION_TIMEDOUT || attempt == 1) break;
        curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
    }
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) { error = curl_easy_strerror(result); return false; }
    if (status < 200 || status >= 300) {
        error = "OpenAI API returned HTTP " + std::to_string(status) + ": " + response;
        return false;
    }
    answer = extractContent(response);
    if (answer.empty()) { error = "Could not read message.content from the OpenAI response"; return false; }
    return true;
}
