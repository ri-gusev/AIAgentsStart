#include "api_client.h"

#include <curl/curl.h>

#include <cctype>
#include <limits>

namespace {
constexpr long kRequestTimeoutSeconds = 90L;
constexpr long kMaxCompletionTokens = 800L;

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

std::string extractJsonStringField(const std::string& json, const std::string& field,
                                   size_t from = 0) {
    const auto key = json.find("\"" + field + "\"", from);
    if (key == std::string::npos) return {};
    auto start = json.find(':', key + field.size() + 2);
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

std::string extractContent(const std::string& json) {
    const auto choices = json.find("\"choices\"");
    if (choices == std::string::npos) return {};
    const auto message = json.find("\"message\"", choices);
    if (message == std::string::npos) return {};
    return extractJsonStringField(json, "content", message);
}

size_t findTopLevelFieldValue(const std::string& json, const std::string& field) {
    int objectDepth = 0;
    for (size_t position = 0; position < json.size(); ++position) {
        if (json[position] == '{') {
            ++objectDepth;
            continue;
        }
        if (json[position] == '}') {
            --objectDepth;
            continue;
        }
        if (json[position] != '"') continue;

        const size_t stringStart = position + 1;
        bool escaped = false;
        bool containsEscape = false;
        size_t stringEnd = stringStart;
        for (; stringEnd < json.size(); ++stringEnd) {
            const char c = json[stringEnd];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                containsEscape = true;
                continue;
            }
            if (c == '"') break;
        }
        if (stringEnd >= json.size()) return std::string::npos;

        if (objectDepth == 1 && !containsEscape &&
            stringEnd - stringStart == field.size() &&
            json.compare(stringStart, field.size(), field) == 0) {
            size_t colon = stringEnd + 1;
            while (colon < json.size() &&
                   std::isspace(static_cast<unsigned char>(json[colon]))) ++colon;
            if (colon < json.size() && json[colon] == ':') return colon + 1;
        }
        position = stringEnd;
    }
    return std::string::npos;
}

size_t findJsonObjectEnd(const std::string& json, size_t objectStart) {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t position = objectStart; position < json.size(); ++position) {
        const char c = json[position];
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return position;
    }
    return std::string::npos;
}

bool extractJsonUnsignedField(const std::string& json, const std::string& field,
                              size_t from, size_t to, std::uint64_t& value) {
    const auto key = json.find("\"" + field + "\"", from);
    if (key == std::string::npos || key >= to) return false;
    const auto colon = json.find(':', key + field.size() + 2);
    if (colon == std::string::npos || colon >= to) return false;
    size_t position = json.find_first_not_of(" \t\r\n", colon + 1);
    if (position == std::string::npos || position >= to ||
        !std::isdigit(static_cast<unsigned char>(json[position]))) {
        return false;
    }

    std::uint64_t parsed = 0;
    while (position < to && std::isdigit(static_cast<unsigned char>(json[position]))) {
        const unsigned digit = static_cast<unsigned>(json[position] - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
        parsed = parsed * 10 + digit;
        ++position;
    }
    value = parsed;
    return true;
}

void extractTokenUsage(const std::string& json, ApiTokenUsage& usage) {
    usage = {};
    const auto usageValue = findTopLevelFieldValue(json, "usage");
    if (usageValue == std::string::npos) return;
    const auto usageStart = json.find_first_not_of(" \t\r\n", usageValue);
    if (usageStart == std::string::npos || json[usageStart] != '{') return;
    const auto usageEnd = findJsonObjectEnd(json, usageStart);
    if (usageEnd == std::string::npos) return;
    extractJsonUnsignedField(json, "prompt_tokens", usageStart, usageEnd, usage.inputTokens);
    extractJsonUnsignedField(json, "completion_tokens", usageStart, usageEnd, usage.outputTokens);
    extractJsonUnsignedField(json, "total_tokens", usageStart, usageEnd, usage.totalTokens);
}

std::string extractFinishReason(const std::string& json) {
    const auto position = json.rfind("\"finish_reason\"");
    return position == std::string::npos
               ? std::string{}
               : extractJsonStringField(json, "finish_reason", position);
}

std::string explainMissingContent(const std::string& json) {
    const auto choices = json.find("\"choices\"");
    const std::string refusal = extractJsonStringField(json, "refusal", choices);
    if (!refusal.empty()) return "Model refusal: " + refusal;

    const std::string finishReason = extractFinishReason(json);
    std::string error = "OpenAI returned HTTP 200 but choices[0].message.content was empty or null";
    if (!finishReason.empty()) error += " (finish_reason=" + finishReason + ")";
    if (finishReason == "length") error += ". The completion token limit was reached.";
    return error;
}
}

ApiClient::ApiClient() : initialized_(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {}
ApiClient::~ApiClient() { if (initialized_) curl_global_cleanup(); }
bool ApiClient::isReady() const { return initialized_; }

bool ApiClient::sendChatCompletion(const std::string& apiKey, const std::string& model,
                                   const std::string& messagesJson, std::string& answer,
                                   std::string& error, bool jsonResponse,
                                   ApiTokenUsage* usage,
                                   std::string* finishReason) const {
    if (usage) *usage = {};
    if (finishReason) finishReason->clear();
    if (!initialized_) { error = "Could not initialize libcurl"; return false; }
    std::string body = "{\"model\":\"" + jsonEscape(model) + "\",\"messages\":" + messagesJson +
                       ",\"max_completion_tokens\":" + std::to_string(kMaxCompletionTokens);
    if (jsonResponse) body += ",\"response_format\":{\"type\":\"json_object\"}";
    body += '}';
    CURL* curl = curl_easy_init();
    if (!curl) { error = "Could not initialize libcurl"; return false; }
    std::string response;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + apiKey).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/chat/completions");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, kRequestTimeoutSeconds);
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
    if (usage) extractTokenUsage(response, *usage);
    if (finishReason) *finishReason = extractFinishReason(response);
    answer = extractContent(response);
    if (answer.empty()) { error = explainMissingContent(response); return false; }
    return true;
}
